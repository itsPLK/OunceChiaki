// OunceChiaki streams on the local network only. chiaki-ng's internet path
// (PSN hole punching, curl certificate recovery) isn't built; session.c only
// reaches these when connect_info.holepunch_session is set, which we never do.
#include <arpa/inet.h>
#include <netinet/in.h>
#ifndef INET6_ADDRSTRLEN
#define INET6_ADDRSTRLEN 46 // not in libounce's headers; holepunch.h needs it
#endif
#include <chiaki/remote/holepunch.h>
#include <errno.h>
#include <stdlib.h>

ChiakiErrorCode chiaki_aia_init(void) { return CHIAKI_ERR_SUCCESS; }

void chiaki_holepunch_session_fini(ChiakiHolepunchSession s) { if(s) abort(); }
void chiaki_holepunch_main_thread_cancel(ChiakiHolepunchSession s, bool stop) { (void)stop; if(s) abort(); }
ChiakiHolepunchRegistInfo chiaki_get_regist_info(ChiakiHolepunchSession s) { (void)s; abort(); }
void chiaki_get_ps_selected_addr(ChiakiHolepunchSession s, char *ip) { (void)s; (void)ip; abort(); }
uint16_t chiaki_get_ps_ctrl_port(ChiakiHolepunchSession s) { (void)s; abort(); }
chiaki_socket_t *chiaki_get_holepunch_sock(ChiakiHolepunchSession s, ChiakiHolepunchPortType t) { (void)s; (void)t; abort(); }
ChiakiErrorCode holepunch_session_create_offer(ChiakiHolepunchSession s) { (void)s; return CHIAKI_ERR_UNINITIALIZED; }
ChiakiErrorCode chiaki_holepunch_session_punch_hole(ChiakiHolepunchSession s, ChiakiHolepunchPortType t) { (void)s; (void)t; return CHIAKI_ERR_UNINITIALIZED; }
