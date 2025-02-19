

#include <sstream>
#include <iostream>
#include <atomic>
#include <strings.h>
#include <sys/stat.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <sys/fsuid.h>
#include <iterator>
#include <mutex>
#include <set>

#include <zlib.h>

#include "hdRDMA.h"

using namespace std;
using std::chrono::duration;
using std::chrono::duration_cast;
using std::chrono::high_resolution_clock;

extern atomic<uint64_t> BYTES_RECEIVED_TOT;
extern atomic<uint64_t> NFILES_RECEIVED_TOT;
extern std::string HDRDMA_REMOTE_ADDR;
extern int VERBOSE;
extern int HDRDMA_RET_VAL;
extern string HDRDMA_USERNAME;
extern string HDRDMA_GROUPNAME;
extern std::mutex HDRDMA_RECV_FNAMES_MUTEX;
extern std::set<string> HDRDMA_RECV_FNAMES;


extern string SendControlCommand(string host, string command);

//
// Some notes on server mode:
//
// - To utilize the full bandwidth of QDR or better IB, multiple
//   streams must be used. It is not enough to post multiple sends
//   to one QP.
//
// - A choice is made here to use reliably connected (RC) connection
//   types. This means a new QP is made for each remote connection.
//
// - We use a separate thread for each remote connection. The thread
//   creates and maintains its own completion queue.
//
// - A single MR is created for all receives, but the memory is broken
//   up into smaller buffers that are maintainined in a pool for use
//   by all threads. This is done so that the "small" buffers can still
//   be quite large, but we can support multiple simultaneous connections.
//


//-----------------------------------------
// hdRDMAThread (constructor)
//-----------------------------------------
hdRDMAThread::hdRDMAThread(hdRDMA *hdrdma)
{
	// Keep copy of pointer to hdRDMA object 
	this->hdrdma = hdrdma;
	
	t1 = high_resolution_clock::now();
	t_last = t1;
}

//-----------------------------------------
// ~hdRDMAThread (destructor)
//-----------------------------------------
hdRDMAThread::~hdRDMAThread()
{
	// Put QP insto RESET state so it releases all outstanding work requests
	if( qp!=nullptr ){
		struct ibv_qp_attr qp_attr;
		bzero( &qp_attr, sizeof(qp_attr) );		
		qp_attr.qp_state = IBV_QPS_RESET;
		ibv_modify_qp (qp, &qp_attr, IBV_QP_STATE);
	}

	// Delete all of our allocated objects
	// n.b. order here matters! If the qp is destroyed after the
	// comp_channel it will leave open a file descriptor pointing
	// to [infinibandevent] that we have no way of closing!
	if(           qp!=nullptr ) ibv_destroy_qp( qp );
	if(           cq!=nullptr ) ibv_destroy_cq ( cq );
	if( comp_channel!=nullptr ) ibv_destroy_comp_channel( comp_channel );
	if(          ofs!=nullptr ) delete ofs;

	// Return MR buffers to pool
	hdrdma->ReturnBuffers( buffers );
}
		
//----------------------------------------------------------------------
// ThreadRun
//
// This is run in a dedicated thread in server mode as soon as a
// TCP connection is established. It will exchange RDMA connection
// information over the given socket and then loop continously until
// the client signals it is done or the "stop" flag is set by the
// hdRDMA object.
//----------------------------------------------------------------------
void hdRDMAThread::ThreadRun(int sockfd)
{
	pthread_setname_np( pthread_self(), "hdRDMAThread::ThreadRun" );

	// The first thing we send via TCP is a 3 byte message indicating
	// success or failure. This really just allows us to inform the client
	// if the server cannot accept another connection right now due to
	// limited RDMA resources.
	// The client will read in 3 bytes from the socket. If they are "OK:"
	// then it knows the next thing to come is the QPInfo structure.
	// If it is "BD:" then it knows then next thing to follow is a message
	// string describing the error.
	
	// This bit of magic ensures that the sockfd is closed and our "stopped" 
	// flag is set before leaving this method, even if early due to error.
	std::shared_ptr<int> x(NULL, [&](int*){ close(sockfd);  stopped=true;});
	
	// Get pool buffers (up to 4). If none are available then tell
	// remote client we have too many RDMA connections.
	hdrdma->GetBuffers(buffers, 4);
	if( buffers.empty() ){
		// No buffers in MR available. Notify remote peer and exit thread
		std::string mess("BD: RDMA server has no more MR buffers (too many connections)");
		cerr << mess << endl;
		write(sockfd, mess.c_str(), mess.length()+1);
		return;
	}

	// Create completion channel and completion queue.
	//
	// TODO:
	// The cq_size can be used to cause an error if too many WR are placed
	// in it. The error would come in the form of an async event (see
	// ibv_get_async_event). If I understand correctly, we would need to 
	// make cq_size smaller than the number of WRs and then check for async
	// errors in a separate thread if we wanted to guarantee that we were
	// processing the data as fast as it is coming in. That adds some
	// significant complication so we skip it for now.
	int cq_size = buffers.size();
	comp_channel = ibv_create_comp_channel( hdrdma->ctx );
	cq = ibv_create_cq( hdrdma->ctx, cq_size, NULL, comp_channel, 0);
	if( !cq ){
		std::stringstream ss;
		ss << "BD: ERROR: Unable to create Completion Queue! errno=" << errno;
		cerr << ss.str() << endl;
		write(sockfd, ss.str().c_str(), ss.str().length()+1);
		return;
	}

	// Tell remote peer we are ready to exchange QPInfo
	std::string mess("OK:");
	write(sockfd, mess.c_str(), mess.length());

	// Exchange QP info over TCP socket so we can transmit via RDMA
	try{
		ExchangeQPInfo( sockfd );
	}catch( Exception &e){
		cerr << e.what() << endl;
		return;
	}

	// Set the filesystem UID/GID if specified by the user. This is primarily for when this is
	// run as root (e.g. as a service) so it can interact with the filesystem as an unpriviliged
	// user.
	SetUIDGID();
	
	// Loop until we're told to stop by either the master thread or the
	// remote peer declaring the connection is closing.
	int num_wc = 1;
	struct ibv_wc wc;
	auto t_last_received = high_resolution_clock::now(); // time we last received a wc
	while( !stop ){
	
		// Check to see if a work completion notification has come in
		int n = ibv_poll_cq(cq, num_wc, &wc);
		if( n<0 ){
			cerr << "ERROR: ibv_poll_cq returned " << n << " - closing connection" << endl;
			break;
		}
		if( n == 0 ){
			std::this_thread::sleep_for(std::chrono::microseconds(1));

			// Timeout if nothing recieved for more than 30 seconds
			auto t_now = high_resolution_clock::now();
			duration<double> duration_since_receive = duration_cast<duration<double>>(t_now - t_last_received);
			auto delta_t = duration_since_receive.count();
			if( delta_t > 30.0 ){
				cout << "TIMEOUT: no RDMA buffers received in more than 30 secs (" << delta_t << "). Closing connection." << endl;
				cout << "         (filename=" << ofilename <<"  Ntransferred=" << Ntransferred << ")" << endl;
				stop = true;
			}

			continue;
		} 
		
		// Work completed!
		if( wc.status != IBV_WC_SUCCESS ){
			cerr << "ERROR: Status of WC not zero (" << wc.status << ") - closing connection" << endl;
			break;
		}
		
		// Make sure this is a IBV_WC_RECV opcode
		if( wc.opcode != IBV_WC_RECV ){
			cerr << "   This is strange... I should only be getting IBV_WC_RECV here! - closing connection" << endl;
			break;
		}
		
		// Process the received data
		auto id = wc.wr_id;
		if( id >= buffers.size() ){
			cerr << "ERROR: Bad id in wc (" << id << ") expected it to be < " << buffers.size() << endl;
			break; // exit thread
		}
		auto &buffer  = buffers[id];
		auto buff     = std::get<0>(buffer);
		//auto buff_len = std::get<1>(buffer);
		BYTES_RECEIVED_TOT += wc.byte_len;
		ReceiveBuffer( buff, wc.byte_len ); //n.b. do NOT use buff_len here!
		t_last_received = high_resolution_clock::now();

		// Re-post the receive request
		PostWR( id );

	} // while( !stop )

}

//-------------------------------------------------------------
// SetUIDGID
//
// This is called to (optionally) set the filesystem uid and gid
// of the process when run in server mode. This is called by
// each hdRDMAThread since the fsuid and fsgid must be set for
// each thread.
//-------------------------------------------------------------
void hdRDMAThread::SetUIDGID(void)
{
	// Note that this calls setfsuid and setfsgid to set the
	// user and group ids when dealing with the filesystem
	// ONLY. This feature is here to allow the server to be
	// started by root, but ensure the creation of files is
	// done as an unpriviliged user.
	//
	// It is worth noting that using seteuid and setreuid were
	// originally tried here, but would cause problems that
	// looked very similar to issues when the memorylocked size
	// was to small. I suspect changing the process IDs caused
	// that limit to change.

	// Create data structures on stack to use in calls to getgrnam_r
	// and getpwnam_r. These are used instead of getgrnam and
	// getpwnam because some problems were seen with files
	// getting assigned strange uids when multiple files were
	// being sent to a server simultaneously. I speculate this
	// was caused by multiple threads simultaneously calling these
	// which, according to the man page, recycle the same memory.
	struct passwd pwd;
	struct passwd *passwd=NULL; // will be set to either NULL or &pwd
	struct group grp;
	struct group *group=NULL;  // will be set to either NULL or &grp
	char buf[8192];
	size_t buflen = 8192;

	// Set effective gid if specified by user
	if( HDRDMA_GROUPNAME.length()>0 ){
		getgrnam_r(HDRDMA_GROUPNAME.c_str(), &grp, buf, buflen, &group);
		//auto group = getgrnam(HDRDMA_GROUPNAME.c_str());
		if( !group ){
			cerr << "Unknown group name \"" << HDRDMA_GROUPNAME << "\"!" << endl;
			exit(-53);
		}
		cout << "Setting fsgid to " << group->gr_gid << " (group=" << group->gr_name << ")" << endl;
		if( setfsgid(group->gr_gid) != 0 ){
			perror("setegid() error");
		}
	}

	// Set effective uid if specified by user
	if( HDRDMA_USERNAME.length()>0 ){
		getpwnam_r(HDRDMA_USERNAME.c_str(), &pwd, buf, buflen, &passwd);
		//auto passwd = getpwnam(HDRDMA_USERNAME.c_str());
		if( !passwd ){
			cerr << "Unknown username \"" << HDRDMA_USERNAME << "\"!" << endl;
			exit(-52);
		}
		if( HDRDMA_GROUPNAME.empty() ){
			// User did not explicitly set group name so set it to default group
			if(VERBOSE>1)cout << "Setting fsgid to " << passwd->pw_gid << " (default for user " << passwd->pw_name << ")" << endl;
			if( setfsgid(passwd->pw_gid) != 0 ){
				perror("setefsgid() error");
			}
		}
		cout << "Setting fsuid to " << passwd->pw_uid << " (username=" << passwd->pw_name << ")" << endl;
		if( setfsuid(passwd->pw_uid) != 0 ){
			perror("setfsuid() error");
		}
	}
}

//-------------------------------------------------------------
// PostWR
//
// Post a receive work request for our QP using the buffer
// parameters associated with the given id.
//-------------------------------------------------------------
void hdRDMAThread::PostWR( int id )
{
	//cout << "Posting WR for id: " << id << endl;

	auto &buffer  = buffers[id];
	auto buff     = std::get<0>(buffer);
	auto buff_len = std::get<1>(buffer);

	struct ibv_recv_wr wr;
	struct ibv_sge sge;
	bzero( &wr, sizeof(wr));
	bzero( &sge, sizeof(sge));
	wr.wr_id = id;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	sge.addr = (uint64_t)buff;
	sge.length = buff_len;
	sge.lkey = hdrdma->mr->lkey;
	auto ret = ibv_post_recv( qp, &wr, &bad_wr);
	if( ret != 0 ){
		cout << "ERROR: ibv_post_recv returned non zero value (" << ret << ")" << endl;
	}
}

//-------------------------------------------------------------
// ExchangeQPInfo
//
// This will create a new QP and send the information to the remote
// peer. It will then receive the QP info from the peer so that the
// two can be linked. It will then call SetToRTS to set the local
// QP to the RTS (Ready To Send) state and RTR (Ready to Receive)
// state.
//-------------------------------------------------------------
void hdRDMAThread::ExchangeQPInfo( int sockfd )
{
	int n;
	struct QPInfo tmp_qp_info;
	
	// Create a new QP to use with the remote peer. 
	CreateQP();
	
	// Create a work receive request for each MR buffer we have
	for( uint32_t id=0; id<buffers.size(); id++ ) PostWR( id );

	tmp_qp_info.lid       = htons(qpinfo.lid);
	tmp_qp_info.qp_num    = htonl(qpinfo.qp_num);
	
	// n.b. we assume below that the remote peer and host pad the QPInfo structure
	// the same. This will be true if we're using the same executable.
   
	//------ Send QPInfo ---------
	n = write(sockfd, (char *)&tmp_qp_info, sizeof(struct QPInfo));
	if( n!= sizeof(struct QPInfo) ){
		std::stringstream ss;
		ss << "ERROR: Sending QPInfo! Tried sending " << sizeof(struct QPInfo) << " bytes but only " << n << " were sent!";
		throw Exception( ss.str() );
	}
    
	//------ Receive QPInfo ---------
	n = read(sockfd, (char *)&tmp_qp_info, sizeof(struct QPInfo));
	if( n!= sizeof(struct QPInfo) ){
		std::stringstream ss;
		ss << "ERROR: Sending QPInfo! Tried reading " << sizeof(struct QPInfo) << " bytes but only " << n << " were read!!";
		throw Exception( ss.str() );
	}

	remote_qpinfo.lid       = ntohs(tmp_qp_info.lid);
	remote_qpinfo.qp_num    = ntohl(tmp_qp_info.qp_num);
    
//	cout << "local    lid: " << qpinfo.lid << "  qp_num: " << qpinfo.qp_num << endl;
//	cout << "remote   lid: " << remote_qpinfo.lid << "  qp_num: " << remote_qpinfo.qp_num << endl;
	
	// Set QP state to RTS
	auto ret = SetToRTS();
	if( ret != 0 ) cout << "ERROR: Unable to set QP to RTS state!" << endl;

}

//-------------------------------------------------------------
// CreateQP
//
// This is used by both client and server modes.
//-------------------------------------------------------------
void hdRDMAThread::CreateQP(void)
{
	// Check if QP already created
	if( qp != nullptr ) return;

	// Set up attributes for creating a QP. 
	struct ibv_qp_init_attr qp_init_attr;
	bzero( &qp_init_attr, sizeof(qp_init_attr) );
	qp_init_attr.send_cq = cq;
	qp_init_attr.recv_cq = cq;
	qp_init_attr.cap.max_send_wr  = 1;
	qp_init_attr.cap.max_recv_wr  = buffers.size();
	qp_init_attr.cap.max_send_sge = 1;
	qp_init_attr.cap.max_recv_sge = 1;
	qp_init_attr.qp_type = IBV_QPT_RC;

	// Create Queue Pair
	qp = ibv_create_qp( hdrdma->pd, &qp_init_attr );
	if( !qp ){
		std::stringstream ss;
		ss << "ERROR: Unable to create QP! errno=" << errno;
		throw Exception(ss.str().c_str());
	}
	
	// Set our QP info so it can be sent to remote hosts when
	// socket connection is established.
	qpinfo.lid    = hdrdma->port_attr.lid;
	qpinfo.qp_num = qp->qp_num;
}

//-------------------------------------------------------------
// SetToRTS
//-------------------------------------------------------------
int hdRDMAThread::SetToRTS(void)
{
#define IB_MTU		IBV_MTU_4096
#define IB_SL		0
	int ret = 0;

	/* change QP state to INIT */
	{
		struct ibv_qp_attr qp_attr;
		bzero( &qp_attr, sizeof(qp_attr) );		
		qp_attr.qp_state        = IBV_QPS_INIT,
		qp_attr.pkey_index      = 0,
		qp_attr.port_num        = hdrdma->port_num,
		qp_attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
	                       IBV_ACCESS_REMOTE_READ |
	                       IBV_ACCESS_REMOTE_ATOMIC |
	                       IBV_ACCESS_REMOTE_WRITE;

		ret = ibv_modify_qp (qp, &qp_attr,
			 IBV_QP_STATE | IBV_QP_PKEY_INDEX |
			 IBV_QP_PORT  | IBV_QP_ACCESS_FLAGS);
		if( ret!=0 ){
			cout << "ERROR: Unable to set QP to INIT state!" << endl;
			return ret;
		}
	}

	/* Change QP state to RTR */
	{
		struct ibv_qp_attr  qp_attr;
		bzero( &qp_attr, sizeof(qp_attr) );		
		qp_attr.qp_state           = IBV_QPS_RTR,
		qp_attr.path_mtu           = IB_MTU,
		qp_attr.dest_qp_num        = remote_qpinfo.qp_num,
		qp_attr.rq_psn             = 0,
		qp_attr.max_dest_rd_atomic = 1,
		qp_attr.min_rnr_timer      = 12,
		qp_attr.ah_attr.is_global  = 0,
		qp_attr.ah_attr.dlid       = remote_qpinfo.lid,
		qp_attr.ah_attr.sl         = IB_SL,
		qp_attr.ah_attr.src_path_bits = 0,
		qp_attr.ah_attr.port_num   = hdrdma->port_num,

		ret = ibv_modify_qp(qp, &qp_attr,
			    IBV_QP_STATE    | IBV_QP_AV |
			    IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
			    IBV_QP_RQ_PSN   | IBV_QP_MAX_DEST_RD_ATOMIC |
			    IBV_QP_MIN_RNR_TIMER);
		if( ret!=0 ){
			cout << "ERROR: Unable to set QP to RTR state!" << endl;
			return ret;
		}
	}

	/* Change QP state to RTS */
	{
		struct ibv_qp_attr  qp_attr;
		bzero( &qp_attr, sizeof(qp_attr) );		
		qp_attr.qp_state      = IBV_QPS_RTS,
		qp_attr.timeout       = 14,
		qp_attr.retry_cnt     = 7,
		qp_attr.rnr_retry     = 7,
		qp_attr.sq_psn        = 0,
		qp_attr.max_rd_atomic = 1,

		ret = ibv_modify_qp (qp, &qp_attr,
			     IBV_QP_STATE | IBV_QP_TIMEOUT |
			     IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
			     IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
		if( ret!=0 ){
			cout << "ERROR: Unable to set QP to RTS state!" << endl;
			return ret;
		}
	}

    return ret;
}

//-------------------------------------------------------------
// ReceiveBuffer
//-------------------------------------------------------------
void hdRDMAThread::ReceiveBuffer(uint8_t *buff, uint32_t buff_len)
{
	hdrdma->Ntransferred += (uint64_t)buff_len;

	auto hi = (HeaderInfo*)buff;
	if( hi->buff_type == 1 ){
		// Buffer holds file information
		if( hi->flags & HI_FIRST_BUFFER ){
			if( ofs != nullptr ) {
				cout << "ERROR: Received new file buffer while file " << ofilename << " already open!" << endl;
				ofs->close();
				delete ofs;
				ofs = nullptr;
			}
			ofilename = (char*)&hi->payload;
			if(VERBOSE>1)cout << "Receiving file: " << ofilename << endl;

			// Create parent directory path if specified by remote sender
			if(VERBOSE>2)cout << "hi->flags: 0x" << std::hex << hi->flags << std::dec << endl;
			if( hi->flags & HI_MAKE_PARENT_DIRS ){
				auto pos = ofilename.find_last_of('/');
				if( pos != std::string::npos ) makePath( ofilename.substr(0, pos) );
			}
			
			ofs = new std::ofstream( ofilename.c_str() );
			ofilesize = 0;
			crcsum = adler32( 0L, Z_NULL, 0 );
			calculate_checksum = (hi->flags & HI_CALCULATE_CHECKSUM); // optionally calculate checksum

			t1 = high_resolution_clock::now();
			t_last = t1; // used for intermediate rate calculations
			delta_t_io = 0.0;
			Ntransferred = 0;

			// Add filename to list of files currently being received
			std::lock_guard<mutex> lck(HDRDMA_RECV_FNAMES_MUTEX);
			HDRDMA_RECV_FNAMES.insert(ofilename);
		}

		if( !ofs ){
			cout << "ERROR: Received file buffer with no file open!" << endl;
			return;
		}
		
		// Write buffer payload to file
		auto data = &buff[hi->header_len];
		auto data_len = buff_len - hi->header_len;
		if( calculate_checksum ) crcsum = adler32( crcsum, data, data_len );
		auto t_io_start = high_resolution_clock::now();
		ofs->write( (const char*)data, data_len );
		auto t_io_end = high_resolution_clock::now();
		duration<double> duration_io = duration_cast<duration<double>>(t_io_end-t_io_start);
		delta_t_io += duration_io.count();
		ofilesize += data_len;
		if( (hi->flags & HI_FIRST_BUFFER) == 0 ) Ntransferred += data_len; // exclude first buffer where we don't have timing info
		
		// If last buffer for file then close it and print stats
		if( hi->flags & HI_LAST_BUFFER ){
			if( t_last != t1 ) cout << endl; // print carriage return if we printed any intermediate progress
			if( ofs ){
				auto t_io_start = high_resolution_clock::now();
				ofs->flush();
				ofs->close();
				auto t_io_end = high_resolution_clock::now();
				duration<double> duration_io = duration_cast<duration<double>>(t_io_end-t_io_start);
				delta_t_io += duration_io.count();
				delete ofs;
				ofs = nullptr;
				NFILES_RECEIVED_TOT++;

				std::lock_guard<mutex> lck(HDRDMA_RECV_FNAMES_MUTEX);
				HDRDMA_RECV_FNAMES.erase(ofilename);
			}
//			auto t2 = high_resolution_clock::now();
//			duration<double> delta_t = duration_cast<duration<double>>(t2-t1);
//			double rate_GBps = (double)Ntransferred/delta_t.count()/1.0E9;
//			double rate_io_GBps = (double)ofilesize/delta_t_io/1.0E9;
//
// 		cout << "  Closed file " << ofilename << " with " << ofilesize/1000000 << " MB" << endl;
// 		cout << "  Transferred the last " << ((double)Ntransferred*1.0E-9) << " GB in " << delta_t.count() << " sec  (" << rate_GBps << " GB/s)" << endl;
// 		cout << "  I/O rate writing to file: " << delta_t_io << " sec  (" << rate_io_GBps << " GB/s)" << endl;
			if( calculate_checksum ) cout << "  checksum: " << std::hex << crcsum << std::dec << endl;
//			cout << "-----------------------------------------------------------" << endl;
			
			// Tell ThreadRun to stop
			stop = true;

		}else{
		
			// Report progress (n.b. this is now reported from hdrdma::Poll() )
			auto t2 = high_resolution_clock::now();
			//duration<double> delta_t = duration_cast<duration<double>>(t2-t_last);
			//double rate_Gbps = (double)buff_len/delta_t.count()*8.0/1.0E9;
			//cout << "\r  received " << buff_len/1000000  << " MB (" << Ntransferred/1000000  << " MB total) - " << rate_Gbps << " Gbps  ";
			//cout.flush();

			t_last = t2;
		}
		
		// Check if flag set indicating the remote side is finished with the connection.
		// n.b. remote client can also send separate message with buff_type==2 for same effect.
		if( hi->flags & HI_LAST_FILE ){
			stop = true;
		}
		
	}else if( hi->buff_type == 2 ){
		// Remote client is done with this connection so tell ThreadRun to stop
		// n.b. remote client can also set flag in hi->flags when buff_type==1 for same effect.
		stop = true;
	}
}

//-------------------------------------------------------------
// ClientConnect
//
// This is called when in client mode to finish setting everything
// up for the connection. The given sockfd will already be connected
// via TCP to the server, but nothing will have been read/written
// yet.
//-------------------------------------------------------------
void hdRDMAThread::ClientConnect( int sockfd )
{
	// This bit of magic ensures that the sockfd is closed and our "stopped" 
	// flag is set before leaving this method, even if early due to error.
	std::shared_ptr<int> x(NULL, [&](int*){ close(sockfd);  stopped=true;});

	// Get pool buffers (all of them). If none are available then throw exception
	hdrdma->GetBuffers( buffers );
	if( buffers.empty() ) throw Exception("ERROR: no MR buffers available in client mode!");

	// Create completion channel and completion queue.
	//
	// TODO:
	// The cq_size can be used to cause an error if too many WR are placed
	// in it. The error would come in the form of an async event (see
	// ibv_get_async_event). If I understand correctly, we would need to 
	// make cq_size smaller than the number of WRs and then check for async
	// errors in a separate thread if we wanted to guarantee that we were
	// processing the data as fast as it is coming in. That adds some
	// significant complication so we skip it for now.
	int cq_size = buffers.size();
	comp_channel = ibv_create_comp_channel( hdrdma->ctx );
	cq = ibv_create_cq( hdrdma->ctx, cq_size, NULL, comp_channel, 0);
	if( !cq ){
		std::stringstream ss;
		ss << "ERROR: Unable to create Completion Queue! errno=" << errno;
		throw Exception(ss.str());
	}
	
	// Set the socket to timeout if unable to read in 10 seconds
	struct timeval tv ={10, 0};
	setsockopt( sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
	
	// Read first 3 bytes from TCP socket to make sure the server is able to
	// send us QPInfo.
	char str[256];
	bzero(str, 256); // status code does not include terminating null
	auto n = read(sockfd, str, 3);
	if( n!= 3 ) throw Exception("ERROR: Unable to read 3 byte status code from TCP socket!" );

	if( std::string(str) != "OK:" ){
		auto n = read(sockfd, str, 256);
		if( n<=0 ) sprintf(str, "Unknown error status from server");
		throw Exception( str );
	}
	
	// Exchange QP info over TCP socket so we can transmit via RDMAcout << __FILE__ << ":" << __LINE__ << endl;
	ExchangeQPInfo( sockfd );

}

//-------------------------------------------------------------
// SendFile
//-------------------------------------------------------------
void hdRDMAThread::SendFile(std::string srcfilename, std::string dstfilename, bool delete_after_send, bool calculate_checksum, bool makeparentdirs)
{
	// Open local file
	std::ifstream ifs(srcfilename.c_str());
	if( !ifs.is_open() ){
		cerr <<"ERROR: Unable to open file \"" << srcfilename << "\"!" << endl;
		exit(-40);
	}
	
	// Get filesize
	ifs.seekg(0, ifs.end);
	auto filesize = ifs.tellg();
	ifs.seekg(0, ifs.beg);
	double filesize_GB = (double)filesize*1.0E-9;
	
	std::string mess = delete_after_send ? " - will be deleted after send":"";
	cout << "Sending file: " << srcfilename << "-> (" << HDRDMA_REMOTE_ADDR << ":)" << dstfilename << "   (" << filesize_GB << " GB)" << mess << endl;
	
	struct ibv_send_wr wr, *bad_wr = nullptr;
	struct ibv_sge sge;
	bzero( &wr, sizeof(wr) );
	bzero( &sge, sizeof(sge) );
	
	wr.opcode = IBV_WR_SEND;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.send_flags = IBV_SEND_SIGNALED,
	
	sge.lkey = hdrdma->mr->lkey;
	
	// Send buffers
	crcsum = adler32( 0L, Z_NULL, 0 );
	t1 = high_resolution_clock::now();
	t_last = t1;
	uint64_t Ntransferred = 0;
	uint64_t bytes_left = filesize;
	uint32_t Noutstanding_writes = 0;
	double delta_t_io = 0.0;
	for(int i=0; i<1000; i++){ // if sending more than 1000 buffers something is wrong!
		auto id = i%buffers.size();
		auto &buffer  = buffers[id];
		auto buff     = std::get<0>(buffer);
		auto buff_len = std::get<1>(buffer);
		sge.addr = (uint64_t)buff;
		HeaderInfo *hi = (HeaderInfo*)sge.addr;
		hi->buff_type = 1; // buffer holds data for file transfer
		hi->flags = 0x0;
		
		// First buffer must contain destination file name.
		// Subsequent buffers don't.
		if( i==0 ){
			hi->header_len = 256;
			hi->flags |= HI_FIRST_BUFFER; // first buffer of file
			if( calculate_checksum ) hi->flags |= HI_CALCULATE_CHECKSUM; // tell remote server to calculate checksum
			if( makeparentdirs     ) hi->flags |= HI_MAKE_PARENT_DIRS;   // tell remote server to make directory path if needed
			sprintf( (char*)&hi->payload, dstfilename.c_str() );
		}else{
			hi->header_len = sizeof(*hi) - sizeof(hi->payload);
		}
		
		// Calculate bytes to be sent in this buffer
		auto bytes_available = buff_len - hi->header_len;
		uint64_t bytes_payload = 0;
		if( bytes_available >= bytes_left ){
			// last buffer of file
			hi->flags |= (HI_LAST_BUFFER | HI_LAST_FILE); // flag as last buffer and last file so connection is closed
			bytes_payload = bytes_left;
		}else{
			// intermediate buffer of file
			bytes_payload = bytes_available;
		}
		
		// Read next block of data directly into mr memory
		auto payload_ptr = &((char*)sge.addr)[hi->header_len];
		auto t_io_start = high_resolution_clock::now();
		ifs.read( payload_ptr, bytes_payload );
		auto t_io_end = high_resolution_clock::now();
		duration<double> duration_io = duration_cast<duration<double>>(t_io_end-t_io_start);
		delta_t_io += duration_io.count();

		// Total length of buffer we are actually sending
		sge.length = hi->header_len + bytes_payload;

		// Optionally calculate cehcksum
		if( calculate_checksum ) crcsum = adler32( crcsum, (uint8_t*)payload_ptr, bytes_payload );
		
		// Post write
		auto ret = ibv_post_send( qp, &wr, &bad_wr );
		if( ret != 0 ){
			cout << "ERROR: ibv_post_send returned non zero value (" << ret << ")" << endl;
			break;
		}
		Noutstanding_writes++;
		Ntransferred += bytes_payload;
		bytes_left -= bytes_payload;

		// Report progress
		auto t2 = high_resolution_clock::now();
		duration<double> delta_t = duration_cast<duration<double>>(t2-t_last);
		double rate_Gbps = (double)sge.length/delta_t.count()*8.0/1.0E9;
		cout << "\r  queued " << sge.length/1000000 << "MB (" << Ntransferred/1000000 << "/" << filesize/1000000  << " MB -- " << (100.0*Ntransferred/filesize) <<"%  - " << rate_Gbps << " Gbps)   ";
		cout.flush();
		
		t_last = t2;

		// If we've posted data using all available sections of the mr
		// then we need to wait for one to finish so we can recycle it.
		if( Noutstanding_writes>=buffers.size() ){
			PollCQ();
			Noutstanding_writes--;
		}
		
		if( hi->flags & HI_LAST_BUFFER ) break; // this was last buffer of file
	}
		
	// Wait for final buffers to transfer
	if( Noutstanding_writes != 0 ) cout << endl;
	while( Noutstanding_writes > 0 ){
		cout << "\r  waiting for final " << Noutstanding_writes << " transfers to complete ...";
		cout .flush();
		PollCQ();
		Noutstanding_writes--;
	}
	cout << endl;

	// Calculate total transfer rate and report.
	auto t2 = high_resolution_clock::now();
	duration<double> delta_t = duration_cast<duration<double>>(t2-t1);
	double rate_Gbps = (double)Ntransferred/delta_t.count()*8.0/1.0E9;
	double rate_io_Gbps = (double)Ntransferred/delta_t_io*8.0/1.0E9;
	//double rate_ib_Gbps = (double)Ntransferred/(delta_t.count()-delta_t_io)*8.0/1.0E9;
	if( Ntransferred>2E8 ){
		cout << "  Transferred " << ((double)Ntransferred*1.0E-9) << " GB in " << delta_t.count() << " sec  (" << rate_Gbps << " Gbps)" << endl;
		cout << "  I/O rate reading from file: " << delta_t_io << " sec  (" << rate_io_Gbps << " Gbps)" << endl;
	}else{
		cout << "  Transferred " << ((double)Ntransferred*1.0E-6) << " MB in " << delta_t.count() << " sec  (" << rate_Gbps*1000.0 << " Mbps)" << endl;
		cout << "  I/O rate reading from file: " << delta_t_io << " sec  (" << rate_io_Gbps*1000.0 << " Mbps)" << endl;
	}	
	if( calculate_checksum ) cout << "  checksum: " << std::hex << crcsum << std::dec << endl;
	//cout << "  IB rate sending file: " << delta_t.count()-delta_t_io << " sec  (" << rate_ib_Gbps << " Gbps) - n.b. don't take this seriously!" << endl;

	// Verify file was completely sent by checking file size on remote host
	string response = SendControlCommand( HDRDMA_REMOTE_ADDR, string("get_file_size ") + dstfilename);
//	cout << "response: " << response << endl;
	std::vector<string> vals;
	std::istringstream iss( response );
	copy( std::istream_iterator<string>(iss), std::istream_iterator<string>(), back_inserter(vals) );
	int64_t fsize = 0;
	if( vals.size()>1 ) fsize = atoll( vals[1].c_str() );
	if( fsize == filesize ){
		cout << "  Confirmed remote file size matches local: " << fsize << " bytes" << endl;
		if( delete_after_send ) {
			unlink(srcfilename.c_str());
			cout << "  Deleted src file: " << srcfilename << endl;
		}
	}else{
		cerr << "Local and remote file sizes do not match after send! (" << filesize << " != " << fsize << ")" << endl;
		cerr << "response from server was: " << response << endl;
		HDRDMA_RET_VAL = -1;
	}
}

//-------------------------------------------------------------
// PollCQ
//
// This is called from SendFile to check for completed send requests.
// It will block until a WR is received or the stop flag is set. Only
// one WR will be pulled from the CQ so the caller may keep track of
// number of outstanding WRs.
//-------------------------------------------------------------
void hdRDMAThread::PollCQ(void)
{
	// Loop until a WR is received
	int num_wc = 1;
	struct ibv_wc wc;
	while( !stop ){
	
		// Check to see if a work completion notification has come in
		int n = ibv_poll_cq(cq, num_wc, &wc);
		if( n<0 ){
			std::stringstream ss;
			ss << "ERROR: ibv_poll_cq returned " << n << " - closing connection";
			throw Exception( ss.str() );
		}
		if( n == 0 ){
			std::this_thread::sleep_for(std::chrono::microseconds(1));
			continue;
		} 
		
		break;
	}
}


//-------------------------------------------------------------
// makePath
//
// Make all directories needed for a given path to exist
//
// (copied from SO question 675039 and streamlined for Linux only)
//-------------------------------------------------------------
bool hdRDMAThread::makePath( const std::string &path )
{
	mode_t mode = 0777;
	int ret = mkdir( path.c_str(), mode );
	if( ret == 0 ) {
		cout << "mkdir: " << path << endl;
		return true;
	}
	
	switch( errno ){
		case ENOENT:
			// parent didn't exist, try to create it
			{
				auto pos = path.find_last_of('/');
				if( pos == std::string::npos ) return false;
				if( !makePath( path.substr(0, pos) ) ) return false;
			}
			// now, try to create again
			return 0 == mkdir( path.c_str(), mode );

		case EEXIST:
			// Path already exists! Verify it is actually a directory
			{
				struct stat info;
				if( stat( path.c_str(), &info) != 0 ) return false;
				return (info.st_mode & S_IFDIR) != 0;
			}

		default:
			return false;
	}
}
