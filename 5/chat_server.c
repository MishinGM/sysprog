#define _POSIX_C_SOURCE 200809L
#include <alloca.h>
#include "chat.h"
#include "chat_server.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);
	return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}
static void *xrealloc(void *p, size_t s){ void *n=realloc(p, s? s:1); if(!n)abort(); return n; }
static char *str_trim(const char *s,size_t l,size_t *out)
{
	const char *b=s,*e=s+l; while(b<e&&isspace((unsigned char)*b))++b;
	while(e>b&&isspace((unsigned char)e[-1]))--e;
	*out=(size_t)(e-b); char *r=malloc(*out+1); memcpy(r,b,*out); r[*out]=0; return r;
}

struct qn{struct chat_message *m; struct qn *n;};
static void q_put(struct qn **h,struct qn **t,struct chat_message *m)
{struct qn *x=malloc(sizeof*x);x->m=m;x->n=NULL;if(*t)(*t)->n=x;else*h=x;*t=x;}
static struct chat_message *q_pop(struct qn **h,struct qn **t)
{struct qn *x=*h;if(!x)return NULL;*h=x->n;if(!*h)*t=NULL;struct chat_message *m=x->m;free(x);return m;}

struct peer{
	int sock;
#if NEED_AUTHOR
	char *name; bool has_name;
#endif
	char *in; size_t icap,ilen;
	char *out;size_t ocap,opos,olen;
	struct peer *next;
};
static struct peer *p_new(int s){struct peer *p=calloc(1,sizeof*p);p->sock=s;return p;}
static void p_del(struct peer *p)
{
	if(p->sock>=0)close(p->sock);
	free(p->in); free(p->out);
#if NEED_AUTHOR
	free(p->name);
#endif
	free(p);
}
static void p_out(struct peer *p,const char *d,size_t l)
{
	if(p->olen+l>p->ocap){p->ocap=(p->olen+l)*2; p->out=xrealloc(p->out,p->ocap);}
	memcpy(p->out+p->olen,d,l); p->olen+=l;
}

struct chat_server{
	int lsock;
	struct peer *peers;
	struct qn   *hq,*tq;
};

struct chat_server *chat_server_new(void)
{
	struct chat_server *s=calloc(1,sizeof*s);
	s->lsock=-1;
	return s;
}
void chat_server_delete(struct chat_server *s)
{
	if(!s)return;
	if(s->lsock>=0)close(s->lsock);
	for(struct peer *p=s->peers,*n; p; p=n){n=p->next;p_del(p);}
	for(struct chat_message *m;(m=q_pop(&s->hq,&s->tq));)chat_message_delete(m);
	free(s);
}

int chat_server_listen(struct chat_server *s,uint16_t port)
{
	if(s->lsock>=0)return CHAT_ERR_ALREADY_STARTED;
	int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0)return CHAT_ERR_SYS;
	set_nonblock(fd);
	int yes=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);
	struct sockaddr_in a={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_ANY),
	                      .sin_port=htons(port)};
	if(bind(fd,(void*)&a,sizeof a)!=0){close(fd);return errno==EADDRINUSE?CHAT_ERR_PORT_BUSY:CHAT_ERR_SYS;}
	if(listen(fd,128)!=0){close(fd);return CHAT_ERR_SYS;}
	s->lsock=fd;
	return 0;
}

static void store(struct chat_server *s,struct peer *from,const char *b,size_t l)
{
	struct chat_message *m=calloc(1,sizeof*m);
#if NEED_AUTHOR
	m->author=strdup(from?from->name:"server");
#endif
	m->data=strndup(b,l);
	q_put(&s->hq,&s->tq,m);
}
static void broadcast(struct chat_server *s,struct peer *from,const char *b,size_t l)
{
	char *wire; size_t wl;
#if NEED_AUTHOR
	const char *an=from?from->name:"server"; size_t al=strlen(an);
	wl=al+1+l+1; wire=malloc(wl);
	memcpy(wire,an,al); wire[al]=' '; memcpy(wire+al+1,b,l); wire[wl-1]='\n';
#else
	wl=l+1; wire=malloc(wl); memcpy(wire,b,l); wire[wl-1]='\n';
#endif
	for(struct peer *p=s->peers;p;p=p->next) if(p!=from) p_out(p,wire,wl);
	free(wire);
}

static void peer_line(struct chat_server *s,struct peer *p,const char *l,size_t n)
{
#if NEED_AUTHOR
	if(!p->has_name){ p->name=strndup(l,n); p->has_name=true; return; }
#endif
	size_t clen; char *clean=str_trim(l,n,&clen);
	if(clen){ store(s,p,clean,clen); broadcast(s,p,clean,clen); }
	free(clean);
}
static void peer_consume(struct chat_server *s,struct peer *p)
{
	char *b=p->in;
	while(true){
		char *nl=memchr(b,'\n',p->ilen-(b-p->in)); if(!nl)break;
		size_t ln=(size_t)(nl-b); if(ln&&b[ln-1]=='\r')--ln;
		peer_line(s,p,b,ln); b=nl+1;
	}
	size_t rem=p->in+p->ilen-b; if(rem&&b!=p->in)memmove(p->in,b,rem);
	p->ilen=rem;
}

int chat_server_get_events(const struct chat_server *s)
{
	if(s->lsock<0)return 0;
	int ev=CHAT_EVENT_INPUT;
	for(struct peer *p=s->peers;p;p=p->next) if(p->olen>p->opos){ev|=CHAT_EVENT_OUTPUT;break;}
	return ev;
}

int chat_server_update(struct chat_server *s,double timeout)
{
	if(s->lsock<0)return CHAT_ERR_NOT_STARTED;
	size_t nfds=1; for(struct peer *p=s->peers;p;p=p->next) ++nfds;
	struct pollfd *pf=alloca(sizeof(struct pollfd)*nfds);

	pf[0]=(struct pollfd){.fd=s->lsock,.events=POLLIN};
	size_t idx=1;
	for(struct peer *p=s->peers;p;p=p->next,++idx){
		pf[idx].fd=p->sock;
		pf[idx].events=POLLIN;
		if(p->olen>p->opos) pf[idx].events|=POLLOUT;
	}

	int tout=timeout<0?-1:(int)(timeout*1000);
	int rc=poll(pf,(nfds_t)nfds,tout);
	if(rc==0){
		for(struct peer *p=s->peers;p;p=p->next)
			if(p->olen>p->opos) return 0;
		return CHAT_ERR_TIMEOUT;
	}
	if(rc<0) return CHAT_ERR_SYS;

	bool progressed=false;

	if(pf[0].revents & POLLIN){
		for(;;){
			int cs=accept(s->lsock,NULL,NULL);
			if(cs<0){
				if(errno==EAGAIN||errno==EWOULDBLOCK)break;
				return CHAT_ERR_SYS;
			}
			set_nonblock(cs);
			struct peer *p=p_new(cs); p->next=s->peers; s->peers=p;
			progressed=true;
		}
	}

	struct peer **pp=&s->peers;
	for(idx=1; *pp; ){
		struct peer *p=*pp; struct pollfd *fd=&pf[idx++];
		bool drop=false;

		if((fd->revents&POLLOUT)&&p->opos<p->olen){
			ssize_t n=send(p->sock,p->out+p->opos,p->olen-p->opos,MSG_NOSIGNAL);
			if(n>0){ p->opos+=n; progressed=true; }
			if(n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK) drop=true;
			if(p->opos==p->olen) p->opos=p->olen=0;
		}

		if(!drop && (fd->revents&POLLIN)){
			if(p->icap-p->ilen<4096){ p->icap=p->icap? p->icap*2: 4096; p->in=xrealloc(p->in,p->icap);}
			ssize_t n=recv(p->sock,p->in+p->ilen,p->icap-p->ilen,0);
			if(n==0) drop=true;
			else if(n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK) drop=true;
			else if(n>0){ p->ilen+=n; peer_consume(s,p); progressed=true; }
		}

		if(drop){
			struct peer *dead=p;
			*pp=p->next;
			p_del(dead);
			progressed=true;
		}else{
			pp=&p->next;
		}
	}

	return progressed ? 0 : CHAT_ERR_TIMEOUT;
}

struct chat_message *chat_server_pop_next(struct chat_server *s){return q_pop(&s->hq,&s->tq);}

int chat_server_feed(struct chat_server *s,const char *buf,uint32_t sz)
{
	if(s->lsock<0)return CHAT_ERR_NOT_STARTED;
	const char *p=buf,*e=buf+sz;
	while(p<e){
		const char *nl=memchr(p,'\n',e-p); if(!nl) nl=e;
		size_t part=(size_t)(nl-p); size_t clen; char *clean=str_trim(p,part,&clen);
		if(clen){ store(s,NULL,clean,clen); broadcast(s,NULL,clean,clen); }
		free(clean);
		p=(nl<e&&*nl=='\n')?nl+1:nl;
	}
	return 0;
}

int chat_server_get_socket(const struct chat_server *s){return s->lsock;}
int chat_server_get_descriptor(const struct chat_server *s){(void)s;return -1;}
