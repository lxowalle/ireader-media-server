#if 1
#include "cstringext.h"
#include "sys/sock.h"
#include "sys/thread.h"
#include "sys/system.h"
#include "sys/path.h"
#include "sys/sync.hpp"
#include "sockutil.h"
// #include "aio-worker.h"
#include "ctypedef.h"
#include "ntp-time.h"
#include "rtp-profile.h"
#include "rtsp-server.h"
// #include "media/ps-file-source.h"
#include "media/h264-file-source.h"
#include "media/h265-file-source.h"
#include "media/rtsp-camera-source.h"
// #include "media/mp4-file-source.h"
#include "rtp-udp-transport.h"
#include "rtp-tcp-transport.h"
#include "rtsp-server-aio.h"
#include "uri-parse.h"
#include "urlcodec.h"
#include "path.h"
#include <map>
#include <memory>
#include "cpm/shared_ptr.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

#if defined(_HAVE_FFMPEG_)
#include "media/ffmpeg-file-source.h"
#include "media/ffmpeg-live-source.h"
#endif

#define UDP_MULTICAST_ADDR "239.0.0.2"
#define UDP_MULTICAST_PORT 6000

// ffplay rtsp://127.0.0.1/vod/video/abc.mp4
// Windows --> d:\video\abc.mp4
// Linux   --> ./video/abc.mp4

#if defined(OS_WINDOWS)
static const char* s_workdir = "d:\\";
#else
static const char* s_workdir = "./";
#endif

static ThreadLocker s_locker;
static RtspCameraSource *camera_source = NULL;
struct rtsp_media_t
{
	std::shared_ptr<IMediaSource> media;
	std::shared_ptr<IRTPTransport> transport;
	uint8_t channel; // rtp over rtsp interleaved channel
	int status; // setup-init, 1-play, 2-pause
	rtsp_server_t* rtsp;
};
typedef std::map<std::string, rtsp_media_t> TSessions;
static TSessions s_sessions;

struct TFileDescription
{
	int64_t duration;
	std::string sdpmedia;
};
static std::map<std::string, TFileDescription> s_describes;

static int rtsp_uri_parse(const char* uri, std::string& path)
{
	char path1[256];
	struct uri_t* r = uri_parse(uri, strlen(uri));
	if(!r)
		return -1;

	url_decode(r->path, strlen(r->path), path1, sizeof(path1));
	path = path1;
	uri_free(r);
	return 0;
}

static int rtsp_ondescribe(void* /*ptr*/, rtsp_server_t* rtsp, const char* uri)
{
	// static const char* pattern_vod =
	// 	"v=0\n"
	// 	"o=- %llu %llu IN IP4 %s\n"
	// 	"s=%s\n"
	// 	"c=IN IP4 0.0.0.0\n"
	// 	"t=0 0\n"
	// 	"a=range:npt=0-%.1f\n"
	// 	"a=recvonly\n"
	// 	"a=control:*\n"; // aggregate control

	// static const char* pattern_live =
	// 	"v=0\n"
	// 	"o=- %llu %llu IN IP4 %s\n"
	// 	"s=%s\n"
	// 	"c=IN IP4 0.0.0.0\n"
	// 	"t=0 0\n"
	// 	"a=range:npt=now-\n" // live
	// 	"a=recvonly\n"
	// 	"a=control:*\n"; // aggregate control

    std::string filename;
	std::map<std::string, TFileDescription>::const_iterator it;

	rtsp_uri_parse(uri, filename);
	if (strstartswith(filename.c_str(), "/live"))
	{
		// living
		filename = filename.c_str() + 5;
	}
	else if (strstartswith(filename.c_str(), "/live/"))
	{
		filename = filename.c_str() + 6;
	}
	else if (strstartswith(filename.c_str(), "/vod/"))
	{
		filename = path::join(s_workdir, filename.c_str() + 5);
	}
	else
	{
		assert(0);
		return -1;
	}

	char buffer[1024] = { 0 };
	{
		AutoThreadLocker locker(s_locker);
		it = s_describes.find(filename);
		if(it == s_describes.end())
		{
			// unlock
			TFileDescription describe;
			std::shared_ptr<IMediaSource> source;
			if (0 == filename.size())
			{
// #if defined(_HAVE_FFMPEG_)
// 				source.reset(new FFLiveSource("video=Integrated Webcam"));
// #endif
				if (!camera_source) {
					camera_source = new RtspCameraSource("output.h265");
				}
				source.reset(camera_source);
				// int offset = snprintf(buffer, sizeof(buffer), pattern_live, ntp64_now(), ntp64_now(), "0.0.0.0", uri);
				// assert(offset > 0 && offset + 1 < sizeof(buffer));
			}
			else
			{
				if (strendswith(filename.c_str(), ".ps"))
				{
					// source.reset(new PSFileSource(filename.c_str()));
				}
				else if (strendswith(filename.c_str(), ".h264"))
					source.reset(new H264FileSource(filename.c_str()));
				else if (strendswith(filename.c_str(), ".h265"))
					source.reset(new H265FileSource(filename.c_str()));
				else
				{
// #if defined(_HAVE_FFMPEG_)
// 					source.reset(new FFFileSource(filename.c_str()));
// #else
// 					source.reset(new MP4FileSource(filename.c_str()));
// #endif
				}
				source->GetDuration(describe.duration);

				// int offset = snprintf(buffer, sizeof(buffer), pattern_vod, ntp64_now(), ntp64_now(), "0.0.0.0", uri, describe.duration / 1000.0);
				// assert(offset > 0 && offset + 1 < sizeof(buffer));
			}

			source->GetSDPMedia(describe.sdpmedia);

			// re-lock
			it = s_describes.insert(std::make_pair(filename, describe)).first;
		}
	}

	std::string sdp = buffer;
	sdp += it->second.sdpmedia;
    return rtsp_server_reply_describe(rtsp, 200, sdp.c_str());
}

static int rtsp_onsetup(void* /*ptr*/, rtsp_server_t* rtsp, const char* uri, const char* session, const struct rtsp_header_transport_t transports[], size_t num)
{
	std::string filename;
	char rtsp_transport[256];
	const struct rtsp_header_transport_t *transport = NULL;

	rtsp_uri_parse(uri, filename);
	if (strstartswith(filename.c_str(), "/live"))
	{
		// living
		filename = filename.c_str() + 5;
	}
	else if (strstartswith(filename.c_str(), "/live/"))
	{
		filename = filename.c_str() + 6;
	}
	else if (strstartswith(filename.c_str(), "/vod/"))
	{
		filename = path::join(s_workdir, filename.c_str() + 5);
	}
	else
	{
		assert(0);
		return -1;
	}

	if ('\\' == *filename.rbegin() || '/' == *filename.rbegin())
		filename.erase(filename.end() - 1);

	if (filename.size() > 0) {
		const char* basename = path_basename(filename.c_str());
		if (NULL == strchr(basename, '.')) // filter track1
			filename.erase(basename - filename.c_str() - 1, std::string::npos);

	}

	TSessions::iterator it;
	if(session)
	{
		AutoThreadLocker locker(s_locker);
		it = s_sessions.find(session);
		if(it == s_sessions.end())
		{
			// 454 Session Not Found
			return rtsp_server_reply_setup(rtsp, 454, NULL, NULL);
		}
		else
		{
			// don't support aggregate control
			if (0)
			{
				// 459 Aggregate Operation Not Allowed
				return rtsp_server_reply_setup(rtsp, 459, NULL, NULL);
			}
		}
	}
	else
	{
		rtsp_media_t item;
		item.rtsp = rtsp;
		item.channel = 0;
		item.status = 0;

		if (0 == filename.size())
		{
// #if defined(_HAVE_FFMPEG_)
// 			item.media.reset(new FFLiveSource("video=Integrated Webcam"));
// #endif
			camera_source = new RtspCameraSource("output.h265");
			item.media.reset(camera_source);
		}
		else
		{
			if (strendswith(filename.c_str(), ".ps"))
			{
				// item.media.reset(new PSFileSource(filename.c_str()));
			}
			else if (strendswith(filename.c_str(), ".h264"))
				item.media.reset(new H264FileSource(filename.c_str()));
			else if (strendswith(filename.c_str(), ".h265"))
				item.media.reset(new H265FileSource(filename.c_str()));
			else
			{
// #if defined(_HAVE_FFMPEG_)
// 				item.media.reset(new FFFileSource(filename.c_str()));
// #else
// 				item.media.reset(new MP4FileSource(filename.c_str()));
// #endif
			}
		}

		char rtspsession[32];
		snprintf(rtspsession, sizeof(rtspsession), "%p", item.media.get());

		AutoThreadLocker locker(s_locker);
		it = s_sessions.insert(std::make_pair(rtspsession, item)).first;
	}

	assert(NULL == transport);
	for(size_t i = 0; i < num && !transport; i++)
	{
		if(RTSP_TRANSPORT_RTP_UDP == transports[i].transport)
		{
			// RTP/AVP/UDP
			transport = &transports[i];
		}
		else if(RTSP_TRANSPORT_RTP_TCP == transports[i].transport)
		{
			// RTP/AVP/TCP
			// 10.12 Embedded (Interleaved) Binary Data (p40)
			transport = &transports[i];
		}
	}
	if(!transport)
	{
		// 461 Unsupported Transport
		return rtsp_server_reply_setup(rtsp, 461, NULL, NULL);
	}

	rtsp_media_t &item = it->second;
	if (RTSP_TRANSPORT_RTP_TCP == transport->transport)
	{
		// 10.12 Embedded (Interleaved) Binary Data (p40)
		int interleaved[2];
		if (transport->interleaved1 == transport->interleaved2)
		{
			interleaved[0] = item.channel++;
			interleaved[1] = item.channel++;
		}
		else
		{
			interleaved[0] = transport->interleaved1;
			interleaved[1] = transport->interleaved2;
		}

		item.transport = std::make_shared<RTPTcpTransport>(rtsp, interleaved[0], interleaved[1]);
		item.media->SetTransport(path_basename(uri), item.transport);

		// RTP/AVP/TCP;interleaved=0-1
		snprintf(rtsp_transport, sizeof(rtsp_transport), "RTP/AVP/TCP;interleaved=%d-%d", interleaved[0], interleaved[1]);
	}
	else if(transport->multicast)
	{
        unsigned short port[2] = { transport->rtp.u.client_port1, transport->rtp.u.client_port2 };
        char multicast[100];
		// RFC 2326 1.6 Overall Operation p12

		if(transport->destination[0])
        {
            // Multicast, client chooses address
            snprintf(multicast, sizeof(multicast), "%s", transport->destination);
            port[0] = transport->rtp.m.port1;
            port[1] = transport->rtp.m.port2;
        }
        else
        {
            // Multicast, server chooses address
            snprintf(multicast, sizeof(multicast), "%s", UDP_MULTICAST_ADDR);
            port[0] = UDP_MULTICAST_PORT;
            port[1] = UDP_MULTICAST_PORT + 1;
        }

        item.transport = std::make_shared<RTPUdpTransport>();
        if(0 != ((RTPUdpTransport*)item.transport.get())->Init(multicast, port))
        {
            // log

            // 500 Internal Server Error
            return rtsp_server_reply_setup(rtsp, 500, NULL, NULL);
        }
        item.media->SetTransport(path_basename(uri), item.transport);

        // Transport: RTP/AVP;multicast;destination=224.2.0.1;port=3456-3457;ttl=16
        snprintf(rtsp_transport, sizeof(rtsp_transport),
            "RTP/AVP;multicast;destination=%s;port=%hu-%hu;ttl=%d",
            multicast, port[0], port[1], 16);

        // 461 Unsupported Transport
        //return rtsp_server_reply_setup(rtsp, 461, NULL, NULL);
	}
	else
	{
		// unicast
		item.transport = std::make_shared<RTPUdpTransport>();

		assert(transport->rtp.u.client_port1 && transport->rtp.u.client_port2);
		unsigned short port[2] = { transport->rtp.u.client_port1, transport->rtp.u.client_port2 };
		const char *ip = transport->destination[0] ? transport->destination : rtsp_server_get_client(rtsp, NULL);
		if(0 != ((RTPUdpTransport*)item.transport.get())->Init(ip, port))
		{
			// log

			// 500 Internal Server Error
			return rtsp_server_reply_setup(rtsp, 500, NULL, NULL);
		}
		item.media->SetTransport(path_basename(uri), item.transport);

		// RTP/AVP;unicast;client_port=4588-4589;server_port=6256-6257;destination=xxxx
		snprintf(rtsp_transport, sizeof(rtsp_transport),
			"RTP/AVP;unicast;client_port=%hu-%hu;server_port=%hu-%hu%s%s",
			transport->rtp.u.client_port1, transport->rtp.u.client_port2,
			port[0], port[1],
			transport->destination[0] ? ";destination=" : "",
			transport->destination[0] ? transport->destination : "");
	}

    return rtsp_server_reply_setup(rtsp, 200, it->first.c_str(), rtsp_transport);
}

static int rtsp_onplay(void* /*ptr*/, rtsp_server_t* rtsp, const char* uri, const char* session, const int64_t *npt, const double *scale)
{
	std::shared_ptr<IMediaSource> source;
	TSessions::iterator it;
	{
		AutoThreadLocker locker(s_locker);
		it = s_sessions.find(session ? session : "");
		if(it == s_sessions.end())
		{
			// 454 Session Not Found
			return rtsp_server_reply_play(rtsp, 454, NULL, NULL, NULL);
		}
		else
		{
			// uri with track
			if (0)
			{
				// 460 Only aggregate operation allowed
				return rtsp_server_reply_play(rtsp, 460, NULL, NULL, NULL);
			}
		}

		source = it->second.media;
	}
	if(npt && 0 != source->Seek(*npt))
	{
		// 457 Invalid Range
		return rtsp_server_reply_play(rtsp, 457, NULL, NULL, NULL);
	}

	if(scale && 0 != source->SetSpeed(*scale))
	{
		// set speed
		assert(*scale > 0);

		// 406 Not Acceptable
		return rtsp_server_reply_play(rtsp, 406, NULL, NULL, NULL);
	}

	// RFC 2326 12.33 RTP-Info (p55)
	// 1. Indicates the RTP timestamp corresponding to the time value in the Range response header.
	// 2. A mapping from RTP timestamps to NTP timestamps (wall clock) is available via RTCP.
	char rtpinfo[512] = { 0 };
	source->GetRTPInfo(uri, rtpinfo, sizeof(rtpinfo));

	// for vlc 2.2.2
	// MP4FileSource* mp4 = dynamic_cast<MP4FileSource*>(source.get());
	// if(mp4)
	// 	mp4->SendRTCP(system_clock());

	it->second.status = 1;
    return rtsp_server_reply_play(rtsp, 200, npt, NULL, rtpinfo);
}

static int rtsp_onpause(void* /*ptr*/, rtsp_server_t* rtsp, const char* /*uri*/, const char* session, const int64_t* /*npt*/)
{
	std::shared_ptr<IMediaSource> source;
	TSessions::iterator it;
	{
		AutoThreadLocker locker(s_locker);
		it = s_sessions.find(session ? session : "");
		if(it == s_sessions.end())
		{
			// 454 Session Not Found
			return rtsp_server_reply_pause(rtsp, 454);
		}
		else
		{
			// uri with track
			if (0)
			{
				// 460 Only aggregate operation allowed
				return rtsp_server_reply_pause(rtsp, 460);
			}
		}

		source = it->second.media;
		it->second.status = 2;
	}

	source->Pause();

	// 457 Invalid Range

    return rtsp_server_reply_pause(rtsp, 200);
}

static int rtsp_onteardown(void* /*ptr*/, rtsp_server_t* rtsp, const char* /*uri*/, const char* session)
{
	std::shared_ptr<IMediaSource> source;
	TSessions::iterator it;
	{
		AutoThreadLocker locker(s_locker);
		it = s_sessions.find(session ? session : "");
		if(it == s_sessions.end())
		{
			// 454 Session Not Found
			return rtsp_server_reply_teardown(rtsp, 454);
		}

		source = it->second.media;
		s_sessions.erase(it);
	}

	return rtsp_server_reply_teardown(rtsp, 200);
}

static int rtsp_onannounce(void* /*ptr*/, rtsp_server_t* rtsp, const char* uri, const char* sdp, int len)
{
    return rtsp_server_reply_announce(rtsp, 200);
}

static int rtsp_onrecord(void* /*ptr*/, rtsp_server_t* rtsp, const char* uri, const char* session, const int64_t *npt, const double *scale)
{
    return rtsp_server_reply_record(rtsp, 200, NULL, NULL);
}

static int rtsp_onoptions(void* ptr, rtsp_server_t* rtsp, const char* uri)
{
	__attribute__((unused)) const char* require = rtsp_server_get_header(rtsp, "Require");
	return rtsp_server_reply_options(rtsp, 200);
}

static int rtsp_ongetparameter(void* ptr, rtsp_server_t* rtsp, const char* uri, const char* session, const void* content, int bytes)
{
	__attribute__((unused)) const char* ctype = rtsp_server_get_header(rtsp, "Content-Type");
	__attribute__((unused)) const char* encoding = rtsp_server_get_header(rtsp, "Content-Encoding");
	 __attribute__((unused)) const char* language = rtsp_server_get_header(rtsp, "Content-Language");
	return rtsp_server_reply_get_parameter(rtsp, 200, NULL, 0);
}

static int rtsp_onsetparameter(void* ptr, rtsp_server_t* rtsp, const char* uri, const char* session, const void* content, int bytes)
{
	__attribute__((unused)) const char* ctype = rtsp_server_get_header(rtsp, "Content-Type");
	__attribute__((unused)) const char* encoding = rtsp_server_get_header(rtsp, "Content-Encoding");
	 __attribute__((unused)) const char* language = rtsp_server_get_header(rtsp, "Content-Language");
	return rtsp_server_reply_set_parameter(rtsp, 200);
}

static int rtsp_onclose(void* /*ptr2*/)
{
	// TODO: notify rtsp connection lost
	//       start a timer to check rtp/rtcp activity
	//       close rtsp media session on expired
	printf("rtsp close\n");
	s_sessions.clear();
	return 0;
}

// static void rtsp_onerror(void* /*param*/, rtsp_server_t* rtsp, int code)
// {
// 	printf("rtsp_onerror code=%d, rtsp=%p\n", code, rtsp);

// 	TSessions::iterator it;
// 	AutoThreadLocker locker(s_locker);
// 	for (it = s_sessions.begin(); it != s_sessions.end(); ++it)
// 	{
// 		if (rtsp == it->second.rtsp)
// 		{
// 			it->second.media->Pause();
// 			s_sessions.erase(it);
// 			break;
// 		}
// 	}

//     //return 0;
// }

#if 1
static int rtsp_send(void* ptr, const void* data, size_t bytes)
{
	socket_t socket = (socket_t)(intptr_t)ptr;

	// TODO: send multiple rtp packet once time
	return (int)bytes == socket_send(socket, data, bytes, 0) ? 0 : -1;
}

extern "C" void rtsp_example()
{
	socket_t socket;
	char buffer[512];

	// create server socket
	socket = socket_tcp_listen(0 /*AF_UNSPEC*/, "0.0.0.0", 8554, SOMAXCONN, 1, 0);
	if (socket_invalid == socket)
		return;

	while(1)
	{
		sockaddr_storage addr;
		socklen_t len = sizeof(addr);
		socket_t tcp = socket_accept(socket, &addr, &len);
		if (socket_invalid == tcp)
			continue;

		struct rtsp_handler_t handler;
		memset(&handler, 0, sizeof(handler));
		handler.ondescribe = rtsp_ondescribe;
		handler.onsetup = rtsp_onsetup;
		handler.onplay = rtsp_onplay;
		handler.onpause = rtsp_onpause;
		handler.onteardown = rtsp_onteardown;
		handler.onannounce = rtsp_onannounce;
		handler.onrecord = rtsp_onrecord;
		handler.onoptions = rtsp_onoptions;
		handler.ongetparameter = rtsp_ongetparameter;
		handler.onsetparameter = rtsp_onsetparameter;
		handler.close = rtsp_onclose;
		handler.send = rtsp_send;

		u_short port = 0;
		socket_setnonblock(tcp, 0); // block io
		socket_addr_to((const sockaddr*)&addr, len, buffer, &port);
		struct rtsp_server_t* rtsp = rtsp_server_create(buffer, port, &handler, NULL, (void*)(intptr_t)tcp); // reuse-able, don't need create in every link

		while (1)
		{
			int r = socket_recv_by_time(tcp, buffer, sizeof(buffer), 0, 5);
			if (r > 0)
			{
				size_t n = r;
				r = rtsp_server_input(rtsp, buffer, &n);
				assert(n == 0 && r == 0);
			}
			else if (r <= 0 && r != SOCKET_TIMEDOUT)
			{
				break;
			}

			TSessions::iterator it;
			AutoThreadLocker locker(s_locker);

			for (it = s_sessions.begin(); it != s_sessions.end(); ++it)
			{
				rtsp_media_t& session = it->second;
				if (1 == session.status) {
					session.media->Play();
				}
			}
		}

		rtsp_server_destroy(rtsp);
		socket_close(tcp);
	}

	socket_close(socket);
}

#else
// #define N_AIO_THREAD 4
// extern "C" void rtsp_example()
// {
// 	aio_worker_init(N_AIO_THREAD);
//
// 	struct aio_rtsp_handler_t handler;
// 	memset(&handler, 0, sizeof(handler));
// 	handler.base.ondescribe = rtsp_ondescribe;
//     handler.base.onsetup = rtsp_onsetup;
//     handler.base.onplay = rtsp_onplay;
//     handler.base.onpause = rtsp_onpause;
//     handler.base.onteardown = rtsp_onteardown;
// 	handler.base.close = rtsp_onclose;
//     handler.base.onannounce = rtsp_onannounce;
//     handler.base.onrecord = rtsp_onrecord;
// 	handler.base.onoptions = rtsp_onoptions;
// 	handler.base.ongetparameter = rtsp_ongetparameter;
// 	handler.base.onsetparameter = rtsp_onsetparameter;
// //	handler.base.send; // ignore
// 	handler.onerror = rtsp_onerror;

// 	// 1. check s_workdir, MUST be end with '/' or '\\'
// 	// 2. url: rtsp://127.0.0.1:8554/vod/<filename>
// 	void* tcp = rtsp_server_listen("0.0.0.0", 8554, &handler, NULL); assert(tcp);
// //	void* udp = rtsp_transport_udp_create(NULL, 554, &handler, NULL); assert(udp);
//
// 	// test only
//     while(1)
//     {
// 		system_sleep(5);

// 		TSessions::iterator it;
// 		AutoThreadLocker locker(s_locker);
// 		for(it = s_sessions.begin(); it != s_sessions.end(); ++it)
// 		{
// 			rtsp_media_t &session = it->second;
// 			if(1 == session.status)
// 				session.media->Play();
// 		}

// 		// TODO: check rtsp session activity
//     }

// 	aio_worker_clean(N_AIO_THREAD);
// 	rtsp_server_unlisten(tcp);
// //	rtsp_transport_udp_destroy(udp);
// }
#endif // N_AIO_THREAD

// #if defined(RTSP_TEST_MAIN)

static int get_ip(char *hw, char ip[16])
{
    struct ifaddrs *ifaddr, *ifa;
    int family, s;
    char host[NI_MAXHOST];

    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return -1;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) {
            continue;
        }

        family = ifa->ifa_addr->sa_family;

        if (family == AF_INET) {
            s = getnameinfo(ifa->ifa_addr, (family == AF_INET) ? sizeof(struct sockaddr_in) :
                            sizeof(struct sockaddr_in6), host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            if (s != 0) {
                printf("getnameinfo() failed: %s\n", gai_strerror(s));
                return -1;
            }

            if (!strcmp(ifa->ifa_name, hw)) {
				strncpy(ip, host, 16);
				freeifaddrs(ifaddr);
				return 0;
            }
        }
    }

    freeifaddrs(ifaddr);
	return -1;
}

#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
	bool rtsp_is_init;
	bool rtsp_is_start;
	socket_t rtsp_socket;
	pthread_t rtsp_id;
	char ip[16];
	int port;
} priv_t;

static priv_t priv;

int rtsp_server_init(char *ip, int port)
{
	socket_t socket;
	if (priv.rtsp_is_init) {
		return 0;
	}

	char new_ip[16] = {0};
	if (ip == NULL) {
		strcpy(new_ip, "0.0.0.0");
	} else {
		strcpy(new_ip, ip);
	}

	// create server socket
	socket = socket_tcp_listen(0 /*AF_UNSPEC*/, new_ip, port, SOMAXCONN, 1, 0);
	if (socket_invalid == socket)
		return -1;

	strcpy(priv.ip, new_ip);
	priv.port = port;

	priv.rtsp_socket = socket;
	priv.rtsp_is_init = true;
	return 0;
}

int rtsp_server_deinit(void)
{
	if (!priv.rtsp_is_init) {
		return 0;
	}

	socket_close(priv.rtsp_socket);

	priv.rtsp_is_init = true;
	return 0;
}

char *rtsp_get_server_ip(void)
{
	return priv.ip;
}

int rtsp_get_server_port(void)
{
	return priv.port;
}

void rtsp_send_h265_data(uint64_t time, uint8_t *data, size_t data_len)
{
	TSessions::iterator it;
	AutoThreadLocker locker(s_locker);
	RtspCameraSource *media = camera_source;
	if (media) {
		media->SetPspFromFrame(data, data_len);
	}

	for (it = s_sessions.begin(); it != s_sessions.end(); ++it)
	{
		rtsp_media_t& session = it->second;
		if(session.status == 1) {
			media->Push(time, (uint8_t *)data, data_len);
		}
	}
}

static void* _rtsp_server_thread(void *args)
{
	(void)args;
	socket_t socket;
	char buffer[512];
	socket = priv.rtsp_socket;
	while(1)
	{
		sockaddr_storage addr;
		socklen_t len = sizeof(addr);
		socket_t tcp = socket_accept(socket, &addr, &len);
		if (socket_invalid == tcp)
			continue;

		struct rtsp_handler_t handler;
		memset(&handler, 0, sizeof(handler));
		handler.ondescribe = rtsp_ondescribe;
		handler.onsetup = rtsp_onsetup;
		handler.onplay = rtsp_onplay;
		handler.onpause = rtsp_onpause;
		handler.onteardown = rtsp_onteardown;
		handler.onannounce = rtsp_onannounce;
		handler.onrecord = rtsp_onrecord;
		handler.onoptions = rtsp_onoptions;
		handler.ongetparameter = rtsp_ongetparameter;
		handler.onsetparameter = rtsp_onsetparameter;
		handler.close = rtsp_onclose;
		handler.send = rtsp_send;

		u_short port = 0;
		socket_setnonblock(tcp, 0); // block io
		socket_addr_to((const sockaddr*)&addr, len, buffer, &port);
		struct rtsp_server_t* rtsp = rtsp_server_create(buffer, port, &handler, NULL, (void*)(intptr_t)tcp); // reuse-able, don't need create in every link

		while (1)
		{
			int r = socket_recv_by_time(tcp, buffer, sizeof(buffer), 0, 5);
			if (r > 0)
			{
				size_t n = r;
				r = rtsp_server_input(rtsp, buffer, &n);
				continue;
			}
			else if (r <= 0 && r != SOCKET_TIMEDOUT)
			{
				break;
			}

			TSessions::iterator it;
			AutoThreadLocker locker(s_locker);
			for (it = s_sessions.begin(); it != s_sessions.end(); ++it)
			{
				rtsp_media_t& session = it->second;
				if (1 == session.status) {
					session.media->Play();
				}
			}
		}

		rtsp_server_destroy(rtsp);
		socket_close(tcp);
	}

	return NULL;
}

int rtsp_server_start(void)
{
	if (!priv.rtsp_is_init) {
		return -1;
	}

	if (priv.rtsp_is_start) {
		return 0;
	}

	camera_source = new RtspCameraSource(NULL);
	if (!camera_source) {
		perror("create rtsp camera source failed!\r\n");
		return -1;
	}

	if (0 != pthread_create(&priv.rtsp_id, NULL, _rtsp_server_thread, NULL)) {
		return -1;
	}

	priv.rtsp_is_start = true;
	return 0;
}

int rtsp_server_stop(void)
{
	if (!priv.rtsp_is_init) {
		return -1;
	}

	if (!priv.rtsp_is_start) {
		return 0;
	}

	if (0 != pthread_cancel(priv.rtsp_id)) {
		return -1;
	}

	if (camera_source) {
		delete camera_source;
	}

	priv.rtsp_is_start = false;

	return 0;
}

#ifdef __cplusplus
}
#endif

std::vector<std::string> rtsp_get_server_urls(void)
{
	char new_ip[16] = {0};

	std::vector<std::string> ip_list;

	if (!strcmp("0.0.0.0", priv.ip)) {
		if (!get_ip((char *)"eth0", new_ip)) {
			ip_list.push_back("rtsp://" + std::string(new_ip) + ":" + std::to_string(priv.port) + "/live");
		}
		if (!get_ip((char *)"usb0", new_ip)) {
			ip_list.push_back("rtsp://" + std::string(new_ip) + ":" + std::to_string(priv.port) + "/live");
		}
		if (!get_ip((char *)"wlan0", new_ip)) {
			ip_list.push_back("rtsp://" + std::string(new_ip) + ":" + std::to_string(priv.port) + "/live");
		}
	} else {
		ip_list.push_back("rtsp://" + std::string(priv.ip) + ":" + std::to_string(priv.port) + "/live");
	}

	return ip_list;
}


static inline const uint8_t* search_start_code(const uint8_t* ptr, const uint8_t* end)
{
    for(const uint8_t *p = ptr; p + 3 < end; p++)
    {
        if(0x00 == p[0] && 0x00 == p[1] && (0x01 == p[2] || (0x00==p[2] && 0x01==p[3])))
            return p;
    }
	return end;
}

static void send_file_data(void)
{
	int m_capacity = 0;
	uint8_t *m_ptr = NULL;
	const char *file = "output.h265";
	FILE* fp = fopen(file, "rb");
    if(fp)
    {
		fseek(fp, 0, SEEK_END);
		m_capacity = ftell(fp);
		fseek(fp, 0, SEEK_SET);

        m_ptr = (uint8_t*)malloc(m_capacity);
		fread(m_ptr, 1, m_capacity, fp);
		fclose(fp);

		size_t count = 0;
		while (1) {
			const uint8_t* end = m_ptr + m_capacity;
			const uint8_t* nalu = search_start_code(m_ptr, end);
			const uint8_t* p = nalu;

			while (p < end)
			{
				const unsigned char* pn = search_start_code(p + 4, end);
				size_t bytes = pn - nalu;

				rtsp_send_h265_data(33 * count++, (uint8_t *)nalu, bytes);

				nalu = pn;
				p = pn;

				usleep(33 * 1000);
			}
		}

		free(m_ptr);
    }
}

int main(int argc, const char* argv[])
{
	printf("rtsp test!\n");
#if 0
	rtsp_example();
#else
	rtsp_server_init(NULL, 8554);
	printf("ip:%s port:%d\r\n", rtsp_get_server_ip(), rtsp_get_server_port());
	std::vector<std::string> urls= rtsp_get_server_urls();
	for (int i = 0; i < (int)urls.size(); i ++) {
		printf("url:%s\r\n", urls[i].c_str());
	}
	rtsp_server_start();

	send_file_data();

	while (1) {
		sleep(1);
	}
	rtsp_server_stop();
	rtsp_server_deinit();
#endif


	return 0;
}
// #endif

#endif // _DEBUG
