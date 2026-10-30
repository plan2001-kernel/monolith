/*
 * spserver: Source Pipeline's Plan 9 rc-httpd CGI handler.
 *
 * The handler is intentionally a file service: it never unpacks, compiles,
 * or executes a submitted source archive.  The only mutable state is under
 * SP_ROOT.  Requests are serialized with an OEXCL|ORCLOSE lock and state is
 * written to alternating generations, so an interrupted write leaves the
 * previous generation readable.
 *
 * Build on 9front with 6c/6l and libsec. rc-httpd must route /sp/api/v1/* to
 * this executable. The live server is not configured by this PoC.
 */
#include <u.h>
#include <libc.h>
#include <libsec.h>

enum {
	Nbody = 8192,
	Nchunk = 32768,
	Maxsource = 128*1024*1024,
	Nagent = 16,
	Ntask = 512,
	MaxFailures = 3,
};

#ifndef SP_LEASE_SECONDS
#define SP_LEASE_SECONDS 300
#endif

#define DefaultRoot "/sys/lib/plan2001/source-pipeline"

typedef struct State State;
typedef struct Agent Agent;
typedef struct Task Task;

struct State {
	ulong generation;
	char phase[16];
	char current[80];
	char currentsha[65];	/* SHA-256 as lower-case hexadecimal. */
	uvlong currentsize;
	char active[80];
	char lease[80];
	long leaseuntil;
	char task[80];
	char base[80];
	char staged[80];
	char stagedsha[65];
	uvlong stagedsize;
	int nextagent;
	int nexttask;
	int failures;
	ulong turns;
	char lastlease[80];
	char lastsource[80];
	char testresult[16];
	char lasterror[160];
};

struct Agent {
	char name[80];
	char token[160];
};

struct Task {
	char id[80];
	char title[256];
};

static char root[512];
static char reqpath[1024];
static char method[16];
static char headers[8192];
static char body[Nbody+1];
static Agent agents[Nagent];
static Task tasks[Ntask];
static int nagent, ntask;

static void
respond(int code, char *reason, char *type, char *p)
{
	int n;
	n = p == nil ? 0 : strlen(p);
	fprint(1, "HTTP/1.1 %d %s\r\n", code, reason);
	if(type != nil)
		fprint(1, "Content-Type: %s\r\n", type);
	fprint(1, "Content-Length: %d\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", n);
	if(n > 0)
		write(1, p, n);
	exits(nil);
}

static void
jsonerror(int code, char *reason, char *message)
{
	char b[2048], q[1024];
	int i, n;
	n = 0;
	for(i = 0; message[i] && n < sizeof q-8; i++){
		if(message[i] == '"' || message[i] == '\\') q[n++] = '\\';
		if((uchar)message[i] < 32) q[n++] = ' ';
		else q[n++] = message[i];
	}
	q[n] = 0;
	snprint(b, sizeof b, "{\"error\":\"%s\"}", q);
	respond(code, reason, "application/json", b);
}

static char*
header(char *name)
{
	char *p, *e;
	int n;
	n = strlen(name);
	for(p = headers; *p; ){
		if(cistrncmp(p, name, n) == 0 && p[n] == ':'){
			p += n+1;
			while(*p == ' ' || *p == '\t') p++;
			e = strchr(p, '\n');
			if(e == nil) e = p+strlen(p);
			while(e > p && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) e--;
			return smprint("%.*s", (int)(e-p), p);
		}
		p = strchr(p, '\n');
		if(p == nil) break;
		p++;
	}
	return nil;
}

/* Extract a flat JSON string/bool/integer property. */
static char*
jsonfield(char *p, char *key)
{
	char pattern[96], *s, *e, *v;
	int n;
	snprint(pattern, sizeof pattern, "\"%s\"", key);
	s = strstr(p, pattern);
	if(s == nil) return nil;
	s += strlen(pattern);
	while(*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
	if(*s++ != ':') return nil;
	while(*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
	if(*s == '"'){
		s++;
		e = s;
		while(*e && (*e != '"' || e[-1] == '\\')) e++;
		if(*e != '"') return nil;
		n = e-s;
	}else{
		e = s;
		while(*e && *e != ',' && *e != '}' && *e != ' ' && *e != '\r' && *e != '\n') e++;
		n = e-s;
	}
	if(n < 0 || n > 1024) return nil;
	v = malloc(n+1);
	if(v == nil) return nil;
	memmove(v, s, n);
	v[n] = 0;
	return v;
}

static int
safeid(char *s)
{
	int i, n;
	n = strlen(s);
	if(n < 1 || n >= 80) return 0;
	for(i=0; i<n; i++)
		if(!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
		     (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_' || s[i] == '.'))
			return 0;
	if(strcmp(s, ".") == 0 || strcmp(s, "..") == 0) return 0;
	return 1;
}

static int
writeall(int fd, void *vp, long n)
{
	uchar *p;
	long m, done;
	p = vp;
	for(done=0; done<n; done += m){
		m = write(fd, p+done, n-done);
		if(m <= 0) return -1;
	}
	return 0;
}

static char*
path(char *tail)
{
	return smprint("%s/%s", root, tail);
}

static int
loadconfig(void)
{
	char *p, *line, *end, *bar, *f[4];
	char *ap, *tp;
	int fd, n, hadnl;

	ap = path("agents.cfg");
	if((fd = open(ap, OREAD)) < 0) return -1;
	p = malloc(32768);
	n = read(fd, p, 32767); close(fd);
	if(n < 0) return -1;
	p[n] = 0;
	nagent = 0;
	for(line=p; *line; line=end){
		end = strchr(line, '\n');
		hadnl = end != nil;
		if(end == nil) end = line+strlen(line);
		if(end == line) { if(!hadnl) break; end++; continue; }
		*end = 0;
		if(line[0] != '#'){
			if(getfields(line, f, 4, 1, " \t") >= 3 && strcmp(f[0], "agent") == 0 && nagent < Nagent){
				strecpy(agents[nagent].name, agents[nagent].name+sizeof agents[nagent].name, f[1]);
				strecpy(agents[nagent].token, agents[nagent].token+sizeof agents[nagent].token, f[2]);
				nagent++;
			}
		}
		if(!hadnl) break;
		end++;
	}
	free(p);

	tp = path("tasks.cfg");
	if((fd = open(tp, OREAD)) < 0) return -1;
	p = malloc(65536);
	n = read(fd, p, 65535); close(fd);
	if(n < 0) return -1;
	p[n] = 0;
	ntask = 0;
	for(line=p; *line; line=end){
		end = strchr(line, '\n');
		hadnl = end != nil;
		if(end == nil) end = line+strlen(line);
		if(end == line) { if(!hadnl) break; end++; continue; }
		*end = 0;
		if(line[0] != '#'){
			bar = strchr(line, '|');
			if(bar != nil && ntask < Ntask){
				*bar++ = 0;
				strecpy(tasks[ntask].id, tasks[ntask].id+sizeof tasks[ntask].id, line);
				strecpy(tasks[ntask].title, tasks[ntask].title+sizeof tasks[ntask].title, bar);
				ntask++;
			}
		}
		if(!hadnl) break;
		end++;
	}
	free(p);
	return nagent > 0 ? 0 : -1;
}

static int
readstatefile(char *name, State *s)
{
	char *p, *line, *end, *f[3], *q;
	int fd, n, hadnl;
	char *fn;
	fn = path(name);
	if((fd = open(fn, OREAD)) < 0) return -1;
	p = malloc(8192);
	n = read(fd, p, 8191); close(fd);
	if(n < 0) return -1;
	p[n] = 0;
	memset(s, 0, sizeof *s);
	for(line=p; *line; line=end){
		end = strchr(line, '\n');
		hadnl = end != nil;
		if(end == nil) end=line+strlen(line);
		*end=0;
		if(getfields(line, f, 3, 1, " \t") >= 2){
			q = f[1];
			if(strcmp(f[0], "magic") == 0 && strcmp(q, "SP0") != 0) { free(p); return -1; }
			if(strcmp(f[0], "generation") == 0) s->generation=atol(q);
			else if(strcmp(f[0], "phase") == 0) strecpy(s->phase,s->phase+sizeof s->phase,q);
			else if(strcmp(f[0], "current") == 0) strecpy(s->current,s->current+sizeof s->current,q);
			else if(strcmp(f[0], "currentsha") == 0) strecpy(s->currentsha,s->currentsha+sizeof s->currentsha,q);
			else if(strcmp(f[0], "currentsize") == 0) s->currentsize=atoll(q);
			else if(strcmp(f[0], "active") == 0) strecpy(s->active,s->active+sizeof s->active,q);
			else if(strcmp(f[0], "lease") == 0) strecpy(s->lease,s->lease+sizeof s->lease,q);
			else if(strcmp(f[0], "leaseuntil") == 0) s->leaseuntil=atol(q);
			else if(strcmp(f[0], "task") == 0) strecpy(s->task,s->task+sizeof s->task,q);
			else if(strcmp(f[0], "base") == 0) strecpy(s->base,s->base+sizeof s->base,q);
			else if(strcmp(f[0], "staged") == 0) strecpy(s->staged,s->staged+sizeof s->staged,q);
			else if(strcmp(f[0], "stagedsha") == 0) strecpy(s->stagedsha,s->stagedsha+sizeof s->stagedsha,q);
			else if(strcmp(f[0], "stagedsize") == 0) s->stagedsize=atoll(q);
			else if(strcmp(f[0], "nextagent") == 0) s->nextagent=atoi(q);
			else if(strcmp(f[0], "nexttask") == 0) s->nexttask=atoi(q);
			else if(strcmp(f[0], "failures") == 0) s->failures=atoi(q);
			else if(strcmp(f[0], "turns") == 0) s->turns=atol(q);
			else if(strcmp(f[0], "lastlease") == 0) strecpy(s->lastlease,s->lastlease+sizeof s->lastlease,q);
			else if(strcmp(f[0], "lastsource") == 0) strecpy(s->lastsource,s->lastsource+sizeof s->lastsource,q);
			else if(strcmp(f[0], "testresult") == 0) strecpy(s->testresult,s->testresult+sizeof s->testresult,q);
			else if(strcmp(f[0], "lasterror") == 0) strecpy(s->lasterror,s->lasterror+sizeof s->lasterror,q);
		}
		if(!hadnl) break;
		end++;
	}
	free(p);
	return s->phase[0] != 0 ? 0 : -1;
}

static int
loadstate(State *s)
{
	State a, b;
	int ra, rb;
	ra = readstatefile("state.0", &a);
	rb = readstatefile("state.1", &b);
	if(ra < 0 && rb < 0) return -1;
	*s = (ra == 0 && (rb < 0 || a.generation >= b.generation)) ? a : b;
	return 0;
}

static int
renameto(char *src, char *dst)
{
	Dir *d;
	char *slash;
	d = dirstat(src);
	if(d == nil) return -1;
	slash = strrchr(dst, '/');
	if(slash != nil) dst = slash+1;
	/* dirstat's strings share its allocation; point at our basename briefly. */
	d->name = dst;
	if(dirwstat(src, d) < 0){ free(d); return -1; }
	free(d);
	return 0;
}

static int
savestate(State *s)
{
	char b[4096], *tmp, *dst;
	int n, fd;
	s->generation++;
	snprint(b, sizeof b,
		"magic SP0\ngeneration %lud\nphase %s\ncurrent %s\ncurrentsha %s\ncurrentsize %llud\n"
		"active %s\nlease %s\nleaseuntil %ld\ntask %s\nbase %s\nstaged %s\nstagedsha %s\nstagedsize %llud\n"
		"nextagent %d\nnexttask %d\nfailures %d\nturns %lud\nlastlease %s\nlastsource %s\ntestresult %s\nlasterror %s\n",
		s->generation, s->phase, s->current, s->currentsha, s->currentsize,
		s->active[0] ? s->active : "-", s->lease[0] ? s->lease : "-", s->leaseuntil,
		s->task[0] ? s->task : "-", s->base[0] ? s->base : "-", s->staged[0] ? s->staged : "-",
		s->stagedsha[0] ? s->stagedsha : "-", s->stagedsize, s->nextagent, s->nexttask,
		s->failures, s->turns, s->lastlease[0] ? s->lastlease : "-", s->lastsource[0] ? s->lastsource : "-",
		s->testresult[0] ? s->testresult : "none", s->lasterror[0] ? s->lasterror : "-");
	tmp = path(smprint("state.tmp.%d", getpid()));
	/* Generation 1 is initially state.0; every update targets the other slot. */
	dst = path(s->generation & 1 ? "state.0" : "state.1");
	/* rc-httpd dispatches read and write requests under different identities.
	 * The state file is not served as a static file; status() omits the lease. */
	if((fd = create(tmp, OWRITE|OEXCL, 0644)) < 0) return -1;
	n = writeall(fd, b, strlen(b));
	close(fd);
	if(n < 0){ remove(tmp); return -1; }
	/* The other slot remains valid throughout this replacement. */
	remove(dst);
	if(renameto(tmp, dst) < 0){ remove(tmp); return -1; }
	return 0;
}

static int
lockstate(void)
{
	char *p;
	int fd;
	p = path("state.lock");
	fd = create(p, OWRITE|OEXCL|ORCLOSE, 0600);
	return fd;
}

static int
authorized(char **who, int *isadmin)
{
	char *a, *p, *line, *end, *tok[4], *auth;
	int fd, n, i, hadnl;
	a = header("Authorization");
	if(a == nil || strncmp(a, "Bearer ", 7) != 0) return -1;
	auth = a+7;
	p = path("agents.cfg");
	if((fd = open(p, OREAD)) < 0) return -1;
	p = malloc(32768); n = read(fd, p, 32767); close(fd);
	if(n < 0) return -1;
	p[n]=0;
	for(line=p; *line; line=end){
		end=strchr(line,'\n'); hadnl=end!=nil; if(end==nil) end=line+strlen(line);
		*end=0;
		memset(tok, 0, sizeof tok);
		if(line[0] && line[0] != '#' && getfields(line,tok,4,1," \t") >= 2){
			if(strcmp(tok[0],"admin")==0 && strcmp(tok[1],auth)==0){
				*who=nil; *isadmin=1; free(p); return -2;
			}
			if(strcmp(tok[0],"agent")==0 && tok[2]!=nil && strcmp(tok[2],auth)==0){
				for(i=0;i<nagent;i++) if(strcmp(agents[i].name,tok[1])==0){
					*who=agents[i].name; *isadmin=0; free(p); return i;
				}
			}
		}
		if(!hadnl) break; end++;
	}
	free(p); return -1;
}

static int
readsmallbody(char *dst, int cap)
{
	char *p;
	int n;
	p=header("Content-Length");
	if(p==nil) return -1;
	n=atoi(p); free(p);
	if(n<0 || n>=cap) return -1;
	if(readn(0,dst,n)!=n) return -1;
	dst[n]=0;
	return n;
}

static int
agentfor(char *name)
{
	int i;
	for(i=0;i<nagent;i++) if(strcmp(agents[i].name,name)==0) return i;
	return -1;
}

static int
validlease(State *s, char *who)
{
	char *p;
	p=header("X-SP-Lease");
	if(p==nil) return 0;
	return strcmp(s->active,who)==0 && strcmp(s->lease,p)==0 && s->leaseuntil>=time(nil);
}

static void
jsonquote(char *out, int cap, char *in)
{
	int n;
	uchar c;
	n=0;
	while(*in && n<cap-8){
		c=*in++;
		if(c=='"' || c=='\\'){out[n++]='\\';out[n++]=c;}
		else if(c=='\n'){out[n++]='\\';out[n++]='n';}
		else if(c=='\r'){out[n++]='\\';out[n++]='r';}
		else if(c=='\t'){out[n++]='\\';out[n++]='t';}
		else if(c<32) out[n++]=' ';
		else out[n++]=c;
	}
	out[n]=0;
}

static void
status(void)
{
	State s;
	char b[4096], err[320], agent[256], order[2048];
	int i, n;
	if(loadstate(&s)<0) jsonerror(503,"Service Unavailable","pipeline state is not initialized");
	jsonquote(err,sizeof err,strcmp(s.lasterror,"-")==0?"":s.lasterror);
	n=snprint(order,sizeof order,"[");
	for(i=0;i<nagent;i++){
		jsonquote(agent,sizeof agent,agents[i].name);
		n+=snprint(order+n,sizeof order-n,"%s\"%s\"",i?",":"",agent);
	}
	snprint(order+n,sizeof order-n,"]");
	snprint(b,sizeof b,
		"{\"state\":\"%s\",\"current\":\"%s\",\"sha256\":\"%s\",\"turns\":%lud,\"active_agent\":\"%s\",\"task\":\"%s\",\"test_result\":\"%s\",\"last_error\":\"%s\",\"generation\":%lud,\"agent_order\":%s}",
		s.phase,s.current,s.currentsha,s.turns,s.active[0]?s.active:"",s.task[0]?s.task:"",s.testresult,err,s.generation,order);
	respond(200,"OK","application/json",b);
}

static void
tasklist(void)
{
	char b[32768], q[1024];
	int i, n;
	n=snprint(b,sizeof b,"{\"tasks\":[");
	for(i=0;i<ntask;i++){
		jsonquote(q,sizeof q,tasks[i].title);
		n+=snprint(b+n,sizeof b-n,"%s{\"id\":\"%s\",\"title\":\"%s\"}",i?",":"",tasks[i].id,q);
	}
	snprint(b+n,sizeof b-n,"]}");
	respond(200,"OK","application/json",b);
}

static void
sourceget(char *who)
{
	State s;
	char *p;
	Dir *d;
	int fd;
	uchar buf[Nchunk];
	long n;
	if(loadstate(&s)<0) jsonerror(503,"Service Unavailable","state unavailable");
	if(who==nil || !validlease(&s,who)) jsonerror(403,"Forbidden","active lease required");
	if(s.current[0]==0 || strcmp(s.current,"-")==0) jsonerror(404,"Not Found","no current source");
	p=path(smprint("source/%s.tar.gz",s.current));
	if((fd=open(p,OREAD))<0) jsonerror(503,"Service Unavailable","current source missing");
	d=dirfstat(fd);
	if(d==nil){close(fd);jsonerror(500,"Internal Server Error","cannot stat source");}
	fprint(1,"HTTP/1.1 200 OK\r\nContent-Type: application/gzip\r\nContent-Length: %llud\r\nX-SP-Source-ID: %s\r\nX-SP-SHA256: %s\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",d->length,s.current,s.currentsha);
	while((n=read(fd,buf,sizeof buf))>0) if(writeall(1,buf,n)<0) break;
	close(fd); free(d); exits(nil);
}

static char*
hexsha(char *file, uvlong *size)
{
	DigestState *ds;
	uchar buf[Nchunk], digest[SHA2_256dlen];
	char hex[SHA2_256dlen*2+1], *p;
	int fd, i;
	long n;
	uvlong total;
	if((fd=open(file,OREAD))<0) return nil;
	ds=nil; total=0;
	while((n=read(fd,buf,sizeof buf))>0){ ds=sha2_256(buf,n,nil,ds); total+=n; }
	close(fd);
	if(n<0) return nil;
	sha2_256(nil,0,digest,ds);
	for(i=0;i<SHA2_256dlen;i++) snprint(hex+i*2,3,"%02ux",digest[i]);
	hex[sizeof hex-1]=0;
	*size=total;
	p=strdup(hex);
	return p;
}

static int
randomlease(char *out, int nout)
{
	uchar raw[24];
	char hex[]="0123456789abcdef";
	int fd, n, i;
	if((fd=open("/dev/random",OREAD))<0) return -1;
	n=readn(fd,raw,16); close(fd);
	if(n!=16 || nout<33) return -1;
	for(i=0;i<16;i++){out[i*2]=hex[raw[i]>>4];out[i*2+1]=hex[raw[i]&15];}
	out[32]=0;
	return 0;
}

static void
claim(int whoindex, char *who)
{
	State s;
	char b[2048], lease[80], title[512];
	int lfd;
	if((lfd=lockstate())<0) jsonerror(409,"Conflict","pipeline is busy; retry shortly");
	if(loadstate(&s)<0){close(lfd);jsonerror(503,"Service Unavailable","state unavailable");}
	if(strcmp(s.phase,"BLOCKED")==0){close(lfd);jsonerror(423,"Locked","pipeline is blocked");}
	if(s.active[0] && strcmp(s.active,"-")!=0 && s.leaseuntil>=time(nil)){
		close(lfd);jsonerror(409,"Conflict","another worker holds the lease");
	}
	if(s.active[0] && strcmp(s.active,"-")!=0){
		remove(path(smprint("source/.%s.part",s.lease)));
		remove(path(smprint("source/.%s.stage",s.lease)));
		strecpy(s.lasterror,s.lasterror+sizeof s.lasterror,"lease_expired");
		s.active[0]=s.lease[0]=s.task[0]=0;
		s.failures++;
		if(s.failures>=MaxFailures){strecpy(s.phase,s.phase+sizeof s.phase,"BLOCKED");s.testresult[0]=0;}
	}
	if(strcmp(s.phase,"BLOCKED")==0){savestate(&s);close(lfd);jsonerror(423,"Locked","lease expired too many times");}
	if(s.nextagent != whoindex){close(lfd);jsonerror(409,"Conflict","it is another agent's turn");}
	if(s.nexttask>=ntask){strecpy(s.phase,s.phase+sizeof s.phase,"IDLE");savestate(&s);close(lfd);respond(204,"No Content",nil,nil);}
	if(randomlease(lease,sizeof lease)<0){close(lfd);jsonerror(500,"Internal Server Error","random lease unavailable");}
	strecpy(s.phase,s.phase+sizeof s.phase,"RUNNING");
	strecpy(s.active,s.active+sizeof s.active,who);
	strecpy(s.lease,s.lease+sizeof s.lease,lease);
	s.leaseuntil=time(nil)+SP_LEASE_SECONDS;
	strecpy(s.task,s.task+sizeof s.task,tasks[s.nexttask].id);
	strecpy(s.base,s.base+sizeof s.base,s.current);
	s.staged[0]=s.stagedsha[0]=0; s.stagedsize=0;
	if(savestate(&s)<0){close(lfd);jsonerror(500,"Internal Server Error","state write failed");}
	jsonquote(title,sizeof title,tasks[s.nexttask].title);
	snprint(b,sizeof b,"{\"agent\":\"%s\",\"lease\":\"%s\",\"expires\":%ld,\"task\":{\"id\":\"%s\",\"title\":\"%s\"},\"base_source\":\"%s\",\"source_sha256\":\"%s\",\"source_url\":\"/sp/api/v1/source\",\"lease_seconds\":%d}",who,s.lease,s.leaseuntil,tasks[s.nexttask].id,title,s.current,s.currentsha,SP_LEASE_SECONDS);
	close(lfd);respond(200,"OK","application/json",b);
}

static void
heartbeat(char *who)
{
	State s;
	int lfd;
	char b[256];
	if((lfd=lockstate())<0) jsonerror(409,"Conflict","pipeline is busy");
	if(loadstate(&s)<0 || !validlease(&s,who)){close(lfd);jsonerror(403,"Forbidden","lease is not active");}
	s.leaseuntil=time(nil)+SP_LEASE_SECONDS;
	if(savestate(&s)<0){close(lfd);jsonerror(500,"Internal Server Error","state write failed");}
	snprint(b,sizeof b,"{\"ok\":true,\"expires\":%ld}",s.leaseuntil);
	close(lfd);respond(200,"OK","application/json",b);
}

static void
stage(char *who)
{
	State s;
	char *cl, *sid, *expected, *tmp, *dst;
	char buf[Nchunk];
	DigestState *ds;
	uchar digest[SHA2_256dlen];
	char hex[SHA2_256dlen*2+1];
	int out, i, lfd;
	long n, left;
	uvlong total;
	cl=header("Content-Length"); sid=header("X-SP-Source-ID"); expected=header("X-SP-SHA256");
	if(cl==nil || sid==nil || expected==nil || !safeid(sid)){free(cl);free(sid);free(expected);jsonerror(400,"Bad Request","Content-Length, source id and SHA-256 required");}
	left=atol(cl); free(cl);
	if(left<1 || left>Maxsource){free(sid);free(expected);jsonerror(413,"Content Too Large","source archive size is invalid");}
	if((lfd=lockstate())<0){free(sid);free(expected);jsonerror(409,"Conflict","pipeline is busy");}
	if(loadstate(&s)<0 || !validlease(&s,who)){close(lfd);free(sid);free(expected);jsonerror(403,"Forbidden","lease is not active");}
	tmp=path(smprint("source/.%s.part",s.lease)); dst=path(smprint("source/.%s.stage",s.lease));
	remove(tmp);
	if((out=create(tmp,OWRITE|OEXCL,0644))<0){close(lfd);free(sid);free(expected);jsonerror(500,"Internal Server Error","cannot stage source");}
	ds=nil; total=0;
	while(left>0){
		n=left>sizeof buf?sizeof buf:left;
		n=readn(0,buf,n);
		if(n<=0){close(out);remove(tmp);close(lfd);free(sid);free(expected);jsonerror(400,"Bad Request","incomplete source body");}
		if(writeall(out,buf,n)<0){close(out);remove(tmp);close(lfd);free(sid);free(expected);jsonerror(500,"Internal Server Error","source write failed");}
		ds=sha2_256((uchar*)buf,n,nil,ds); left-=n; total+=n;
	}
	close(out);
	sha2_256(nil,0,digest,ds);
	for(i=0;i<SHA2_256dlen;i++) snprint(hex+i*2,3,"%02ux",digest[i]);
	hex[sizeof hex-1]=0;
	if(cistrcmp(hex,expected)!=0){remove(tmp);close(lfd);free(sid);free(expected);jsonerror(422,"Unprocessable Entity","source SHA-256 mismatch");}
	remove(dst);
	if(renameto(tmp,dst)<0){remove(tmp);close(lfd);free(sid);free(expected);jsonerror(500,"Internal Server Error","cannot finalize staged file");}
	strecpy(s.staged,s.staged+sizeof s.staged,sid);
	strecpy(s.stagedsha,s.stagedsha+sizeof s.stagedsha,hex);
	s.stagedsize=total;
	if(savestate(&s)<0){close(lfd);free(sid);free(expected);jsonerror(500,"Internal Server Error","state write failed");}
	free(sid);free(expected);
	close(lfd);
	respond(201,"Created","application/json","{\"staged\":true}");
}

static void
complete(char *who)
{
	State s;
	char *result, *src, *dst, *actual, *leaseheader, b[512];
	int lfd;
	uvlong size;
	if(readsmallbody(body,sizeof body)<0) jsonerror(400,"Bad Request","invalid completion JSON");
	result=jsonfield(body,"test_result");
	if(result==nil || strcmp(result,"passed")!=0){free(result);jsonerror(422,"Unprocessable Entity","tests must pass before completion");}
	free(result);
	if((lfd=lockstate())<0) jsonerror(409,"Conflict","pipeline is busy");
	if(loadstate(&s)<0){close(lfd);jsonerror(503,"Service Unavailable","state unavailable");}
	leaseheader=header("X-SP-Lease");
	if(leaseheader!=nil && strcmp(s.lastlease,leaseheader)==0 && s.lastsource[0] && strcmp(s.lastsource,s.current)==0){
		free(leaseheader);
		close(lfd);respond(200,"OK","application/json","{\"complete\":true,\"idempotent\":true}");
	}
	free(leaseheader);
	if(!validlease(&s,who)){close(lfd);jsonerror(403,"Forbidden","lease is not active");}
	if(s.staged[0]==0 || strcmp(s.staged,"-")==0){close(lfd);jsonerror(409,"Conflict","no staged source for this lease");}
	src=path(smprint("source/.%s.stage",s.lease));
	dst=path(smprint("source/%s.tar.gz",s.staged));
	/* Recover if the package rename succeeded but the state write did not. */
	if(access(dst,AEXIST)==0){
		actual=hexsha(dst,&size);
		if(actual==nil || strcmp(actual,s.stagedsha)!=0 || size!=s.stagedsize){
			free(actual);close(lfd);jsonerror(409,"Conflict","source id exists with different content");
		}
		free(actual);
	}else if(renameto(src,dst)<0){close(lfd);jsonerror(500,"Internal Server Error","cannot publish source package");}
	strecpy(s.current,s.current+sizeof s.current,s.staged);
	strecpy(s.currentsha,s.currentsha+sizeof s.currentsha,s.stagedsha);
	s.currentsize=s.stagedsize;
	strecpy(s.lastlease,s.lastlease+sizeof s.lastlease,s.lease);
	strecpy(s.lastsource,s.lastsource+sizeof s.lastsource,s.current);
	strecpy(s.testresult,s.testresult+sizeof s.testresult,"passed");
	s.active[0]=s.lease[0]=s.task[0]=s.base[0]=s.staged[0]=s.stagedsha[0]=0;
	s.leaseuntil=0;s.stagedsize=0;s.failures=0;s.turns++;
	s.nextagent=(s.nextagent+1)%nagent;
	s.nexttask++;
	if(s.nexttask>=ntask) strecpy(s.phase,s.phase+sizeof s.phase,"IDLE");
	else strecpy(s.phase,s.phase+sizeof s.phase,"RUNNING");
	if(savestate(&s)<0){close(lfd);jsonerror(500,"Internal Server Error","published source but state update failed; recovery required");}
	snprint(b,sizeof b,"{\"complete\":true,\"source_id\":\"%s\",\"sha256\":\"%s\",\"turns\":%lud}",s.current,s.currentsha,s.turns);
	close(lfd);respond(200,"OK","application/json",b);
}

static void
failwork(char *who)
{
	State s;
	char *msg;
	int lfd, i, n;
	if(readsmallbody(body,sizeof body)<0) jsonerror(400,"Bad Request","invalid failure JSON");
	msg=jsonfield(body,"message");
	if((lfd=lockstate())<0){free(msg);jsonerror(409,"Conflict","pipeline is busy");}
	if(loadstate(&s)<0 || !validlease(&s,who)){close(lfd);free(msg);jsonerror(403,"Forbidden","lease is not active");}
	s.failures++;
	n=0;
	if(msg!=nil) for(i=0;msg[i] && n<sizeof s.lasterror-1;i++){
		if((msg[i]>='a' && msg[i]<='z') || (msg[i]>='A' && msg[i]<='Z') ||
		   (msg[i]>='0' && msg[i]<='9') || msg[i]=='_' || msg[i]=='-' || msg[i]=='.')
			s.lasterror[n++]=msg[i];
		else if(n==0 || s.lasterror[n-1]!='_') s.lasterror[n++]='_';
	}
	if(n==0) strecpy(s.lasterror,s.lasterror+sizeof s.lasterror,"unspecified_failure");
	else s.lasterror[n]=0;
	remove(path(smprint("source/.%s.part",s.lease)));
	remove(path(smprint("source/.%s.stage",s.lease)));
	s.active[0]=s.lease[0]=s.task[0]=0;s.leaseuntil=0;
	s.testresult[0]=0;
	if(s.failures>=MaxFailures) strecpy(s.phase,s.phase+sizeof s.phase,"BLOCKED");
	else strecpy(s.phase,s.phase+sizeof s.phase,"RUNNING");
	if(savestate(&s)<0){close(lfd);free(msg);jsonerror(500,"Internal Server Error","state write failed");}
	close(lfd);free(msg);
	respond(200,"OK","application/json",s.failures>=MaxFailures?"{\"state\":\"BLOCKED\"}":"{\"state\":\"RUNNING\",\"retry\":true}");
}

static void
addtask(int admin)
{
	char *id, *title, *p, *line;
	int fd, i;
	if(!admin) jsonerror(403,"Forbidden","admin token required");
	if(readsmallbody(body,sizeof body)<0) jsonerror(400,"Bad Request","invalid task JSON");
	id=jsonfield(body,"id"); title=jsonfield(body,"title");
	if(id==nil || title==nil || !safeid(id) || strlen(title)>240 || ntask>=Ntask){free(id);free(title);jsonerror(400,"Bad Request","valid task id and title required");}
	for(i=0;i<ntask;i++) if(strcmp(tasks[i].id,id)==0){free(id);free(title);jsonerror(409,"Conflict","task id already exists");}
	for(i=0;title[i];i++) if(title[i]=='|' || title[i]=='\n' || title[i]=='\r'){free(id);free(title);jsonerror(400,"Bad Request","title contains a forbidden character");}
	p=path("tasks.cfg");
	if((fd=open(p,OWRITE))<0){free(id);free(title);jsonerror(500,"Internal Server Error","cannot open task queue");}
	seek(fd,0,2);
	line=smprint("%s|%s\n",id,title);
	if(writeall(fd,line,strlen(line))<0){close(fd);free(id);free(title);jsonerror(500,"Internal Server Error","cannot append task");}
	close(fd);free(id);free(title);
	if(loadconfig()<0) jsonerror(500,"Internal Server Error","cannot reload task queue");
	respond(201,"Created","application/json","{\"queued\":true}");
}

void
main(int argc, char **argv)
{
	char *r, *p, *q, *who;
	int idx, admin, lfd;
	USED(argc); USED(argv);
	r=getenv("request"); p=getenv("reqlines");
	if(r==nil || p==nil) jsonerror(400,"Bad Request","missing rc-httpd request metadata");
	strecpy(headers,headers+sizeof headers,p);
	p=strchr(r,' '); if(p==nil) jsonerror(400,"Bad Request","bad request line");
	*p=0; strecpy(method,method+sizeof method,r);
	q=p+1; p=strchr(q,' '); if(p!=nil) *p=0;
	strecpy(reqpath,reqpath+sizeof reqpath,q);
	if((p=strchr(reqpath,'?'))!=nil) *p=0;
	r=getenv("SP_ROOT");
	strecpy(root,root+sizeof root,r==nil?DefaultRoot:r);
	if(loadconfig()<0) jsonerror(503,"Service Unavailable","agent or task configuration unavailable");
	who=nil;admin=0;
	if(strcmp(method,"GET")==0 && strcmp(reqpath,"/sp/api/v1/status")==0) status();
	if(strcmp(method,"GET")==0 && strcmp(reqpath,"/sp/api/v1/tasks")==0) tasklist();
	if(strcmp(method,"GET")==0 && strcmp(reqpath,"/sp/api/v1/source")==0){
		idx=authorized(&who,&admin); if(idx<0) jsonerror(401,"Unauthorized","agent authentication required");
		sourceget(who);
	}
	if(strncmp(method,"POST",4)!=0) jsonerror(405,"Method Not Allowed","unsupported method");
	idx=authorized(&who,&admin);
	if(idx==-1) jsonerror(401,"Unauthorized","valid bearer token required");
	if(strcmp(reqpath,"/sp/api/v1/tasks")==0){
		if((lfd=lockstate())<0) jsonerror(409,"Conflict","pipeline is busy");
		addtask(admin); close(lfd);
	}
	if(idx<0) jsonerror(403,"Forbidden","agent token required");
	if(strcmp(reqpath,"/sp/api/v1/claim")==0) claim(idx,who);
	if(strcmp(reqpath,"/sp/api/v1/heartbeat")==0) heartbeat(who);
	if(strcmp(reqpath,"/sp/api/v1/source")==0) stage(who);
	if(strcmp(reqpath,"/sp/api/v1/complete")==0) complete(who);
	if(strcmp(reqpath,"/sp/api/v1/fail")==0) failwork(who);
	jsonerror(404,"Not Found","unknown Source Pipeline endpoint");
}
