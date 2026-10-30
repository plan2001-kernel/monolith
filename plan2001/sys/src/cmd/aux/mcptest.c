/*
 * mcptest - a tiny, stateless MCP Streamable HTTP endpoint for testing
 * ChatGPT's remote MCP connection.  It exposes connection and C compile
 * checks, keeps no user data, and never executes submitted binaries.
 * rc-httpd invokes it for POST /mcp and /mpc.
 */
#include <u.h>
#include <libc.h>

enum {
	Maxbody = 64*1024,
	Maxsource = 16*1024,
	Maxoutput = 8192,
	Maxjson = 56*1024,
	Compilems = 10000,
};

#define Lockdir "/sys/lib/plan2001/mcptest-tmp"

static char body[Maxbody+1];
static char replybuf[Maxjson];
static char source[Maxsource+1];
static char compileout[Maxoutput+1];
static char compilemsg[Maxoutput+256];
static char quoted[Maxoutput*6+2048];
static int timedout;
static int activepid;

static int
alarmnote(void*, char *msg)
{
	if(strcmp(msg, "alarm") == 0){
		timedout = 1;
		if(activepid > 0)
			postnote(PNGROUP, activepid, "kill");
		return 1;
	}
	return 0;
}

static void
http(int status, char *reason, char *type, char *p)
{
	int n;

	n = strlen(p);
	fprint(1, "HTTP/1.1 %d %s\r\n", status, reason);
	if(type != nil)
		fprint(1, "Content-Type: %s\r\n", type);
	fprint(1, "Content-Length: %d\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", n);
	if(n > 0)
		write(1, p, n);
	exits(nil);
}

/* Return the JSON value following a top-level key, preserving its spelling. */
static char*
value(char *json, char *key)
{
	char pattern[80], *p, *e;
	int n, quoted, escaped;

	snprint(pattern, sizeof pattern, "\"%s\"", key);
	p = strstr(json, pattern);
	if(p == nil)
		return nil;
	p += strlen(pattern);
	while(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	if(*p++ != ':')
		return nil;
	while(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	e = p;
	quoted = *p == '"';
	if(quoted){
		p++;
		escaped = 0;
		while(*p){
			if(!escaped && *p == '"'){
				p++;
				break;
			}
			if(!escaped && *p == '\\')
				escaped = 1;
			else
				escaped = 0;
			p++;
		}
	}else{
		while(*p && *p != ',' && *p != '}' && *p != ' ' && *p != '\r' && *p != '\n')
			p++;
	}
	n = p-e;
	if(n <= 0 || n > 256)
		return nil;
	return smprint("%.*s", n, e);
}

static char*
headervalue(char *headers, char *name)
{
	char *p, *e;
	int n;

	n = strlen(name);
	for(p = headers; *p; ){
		if(cistrncmp(p, name, n) == 0 && p[n] == ':'){
			p += n+1;
			while(*p == ' ' || *p == '\t')
				p++;
			e = strchr(p, '\n');
			if(e == nil)
				e = p + strlen(p);
			while(e > p && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
				e--;
			return smprint("%.*s", (int)(e-p), p);
		}
		if((p = strchr(p, '\n')) == nil)
			break;
		p++;
	}
	return nil;
}

/* Decode a JSON string field into dst. Reject NUL and malformed escapes. */
static int
stringfield(char *json, char *key, char *dst, int cap)
{
	char pattern[80], *p;
	ulong rune;
	int n, i, h;

	snprint(pattern, sizeof pattern, "\"%s\"", key);
	p = strstr(json, pattern);
	if(p == nil)
		return -1;
	p += strlen(pattern);
	while(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
	if(*p++ != ':')
		return -1;
	while(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
	if(*p++ != '"')
		return -1;
	n = 0;
#define JSONPUT(c) do{ if(n+1 >= cap) return -2; dst[n++] = (c); }while(0)
	while(*p && *p != '"'){
		if(*p == '\\'){
			p++;
			switch(*p++){
			case '"': JSONPUT('"'); break;
			case '\\': JSONPUT('\\'); break;
			case '/': JSONPUT('/'); break;
			case 'b': JSONPUT('\b'); break;
			case 'f': JSONPUT('\f'); break;
			case 'n': JSONPUT('\n'); break;
			case 'r': JSONPUT('\r'); break;
			case 't': JSONPUT('\t'); break;
			case 'u':
				rune = 0;
				for(i = 0; i < 4; i++){
					h = *p++;
					if(h >= '0' && h <= '9') h -= '0';
					else if(h >= 'a' && h <= 'f') h = h-'a'+10;
					else if(h >= 'A' && h <= 'F') h = h-'A'+10;
					else return -1;
					rune = (rune<<4) | h;
				}
				if(rune == 0) return -1;
				if(rune < 0x80){
					JSONPUT(rune);
				}else if(rune < 0x800){
					JSONPUT(0xC0 | (rune>>6));
					JSONPUT(0x80 | (rune&0x3F));
				}else{
					JSONPUT(0xE0 | (rune>>12));
					JSONPUT(0x80 | ((rune>>6)&0x3F));
					JSONPUT(0x80 | (rune&0x3F));
				}
				break;
			default: return -1;
			}
		}else{
			if((uchar)*p < 0x20)
				return -1;
			JSONPUT(*p++);
		}
	}
	if(*p != '"')
		return -1;
	dst[n] = 0;
#undef JSONPUT
	return n;
}

static void
jsontext(char *dst, int cap, char *src)
{
	char *p;
	int left;

	left = cap;
	p = dst;
	while(*src && left > 8){
		if(*src == '"' || *src == '\\'){
			*p++ = '\\'; *p++ = *src++; left -= 2;
		}else if((uchar)*src < 0x20){
			snprint(p, left, "\\u%04x", (uchar)*src++);
			p += 6; left -= 6;
		}else{
			*p++ = *src++; left--;
		}
	}
	*p = 0;
}

static int
writeall(int fd, char *p, int n)
{
	int m, done;

	for(done = 0; done < n; done += m){
		m = write(fd, p+done, n-done);
		if(m <= 0)
			return -1;
	}
	return 0;
}

/* Compile and link in a networkless, device-masked namespace. */
static char*
checkc(char *code, int ncode)
{
	char srcname[48], objname[48], binname[48], cmd[512];
	char chunk[1024];
	char statusmsg[256];
	int lock, in[2], out[2], pid, n, used, truncated, status;
	Waitmsg *w;

	if((lock = create(Lockdir "/compile.lock", OWRITE|ORCLOSE|OEXCL, 0600)) < 0)
		return smprint("Could not acquire the compiler lock (busy or scratch path unavailable): %r");
	snprint(srcname, sizeof srcname, "code.%d.c", getpid());
	snprint(objname, sizeof objname, "code.%d.6", getpid());
	snprint(binname, sizeof binname, "check.%d", getpid());
	if(pipe(in) < 0){
		close(lock);
		return smprint("Could not create a compiler input pipe: %r");
	}
	if(pipe(out) < 0){
		close(in[0]); close(in[1]); close(lock);
		return smprint("Could not create a compiler output pipe: %r");
	}
	snprint(cmd, sizeof cmd,
		". /rc/bin/websession -f; cat > /tmp/%s && cd /tmp && /amd64/bin/6c -FTVw %s && /amd64/bin/6l -o %s %s",
		srcname, srcname, binname, objname);
	switch(pid = rfork(RFPROC|RFFDG|RFNAMEG|RFNOTEG)){
	case -1:
		close(in[0]); close(in[1]); close(out[0]); close(out[1]); close(lock);
		return smprint("Could not start the compiler: %r");
	case 0:
		close(in[1]); close(out[0]);
		dup(in[0], 0); dup(out[1], 1); dup(out[1], 2);
		close(in[0]); close(out[1]); close(lock);
		switch(rfork(RFPROC|RFFDG|RFNOWAIT)){
		case -1: _exits("ramfs fork failed");
		case 0:
			close(0); open("/dev/null", OREAD);
			close(1); open("/dev/null", OWRITE); dup(1, 2);
			execl("/amd64/bin/ramfs", "ramfs", "-p", "-m", "/tmp", nil);
			_exits("ramfs exec failed");
		}
		sleep(1000);
		execl("/bin/rc", "rc", "-c", cmd, nil);
		_exits("could not start rc");
	}
	close(in[0]); close(out[1]);
	used = 0;
	truncated = 0;
	statusmsg[0] = 0;
	timedout = 0;
	activepid = pid;
	atnotify(alarmnote, 1);
	alarm(Compilems);
	if(writeall(in[1], code, ncode) < 0){
		postnote(PNGROUP, pid, "kill");
		truncated = 1;
	}
	close(in[1]);
	for(;;){
		n = read(out[0], chunk, sizeof chunk);
		if(n < 0){
			if(!timedout)
				truncated = 1;
			break;
		}
	if(n == 0)
				break;
		if(used + n > Maxoutput){
			n = Maxoutput-used;
			truncated = 1;
		}
		if(n > 0){ memmove(compileout+used, chunk, n); used += n; }
		if(truncated){
			postnote(PNGROUP, pid, "kill");
			break;
		}
	}
	compileout[used] = 0;
	alarm(0);
	close(out[0]);
	postnote(PNGROUP, pid, "kill");
	status = 1;
	while((w = wait()) != nil){
		if(w->pid == pid){
			status = w->msg[0] == 0;
			strecpy(statusmsg, statusmsg+sizeof statusmsg, w->msg);
			free(w);
			break;
		}
		free(w);
	}
	activepid = 0;
	close(lock);
	if(timedout)
		return strdup("Compilation stopped after the 10 second time limit.");
	if(truncated)
		return smprint("Compiler output exceeded %d bytes and was stopped.\n%s", Maxoutput, compileout);
	if(status && used == 0)
		snprint(compilemsg, sizeof compilemsg, "Compilation and linking succeeded with 6c and 6l.");
	else if(status)
		snprint(compilemsg, sizeof compilemsg, "Compilation and linking succeeded. Compiler diagnostics:\n%s", compileout);
	else if(used == 0)
		snprint(compilemsg, sizeof compilemsg, "Compilation failed: %s", statusmsg);
	else
		snprint(compilemsg, sizeof compilemsg, "Compilation or linking failed (%s). Diagnostics:\n%s", statusmsg, compileout);
	return strdup(compilemsg);
}

void
main(int argc, char **argv)
{
	char *request, *headers, *lenstr, *id, *rpcmethod, *tool, *message;
	long n;

	USED(argc);
	USED(argv);
	tool = nil;
	request = getenv("request");
	headers = getenv("reqlines");
	if(request == nil || headers == nil)
		http(400, "Bad Request", "text/plain", "missing HTTP request metadata\n");
	if(strncmp(request, "POST ", 5) != 0)
		http(405, "Method Not Allowed", "text/plain", "use POST with an MCP JSON-RPC message\n");
	lenstr = headervalue(headers, "Content-Length");
	if(lenstr == nil)
		http(413, "Content Too Large", "text/plain", "invalid or oversized request body\n");
	n = atoi(lenstr);
	if(n <= 0 || n > Maxbody)
		http(413, "Content Too Large", "text/plain", "invalid or oversized request body\n");
	if(readn(0, body, n) != n)
		http(400, "Bad Request", "text/plain", "incomplete request body\n");
	body[n] = 0;
	id = value(body, "id");
	rpcmethod = value(body, "method");
	if(rpcmethod == nil){
		free(id);
		http(400, "Bad Request", "text/plain", "invalid JSON-RPC request\n");
	}
	/* MCP notifications do not have an id and must not receive a JSON body. */
	if(id == nil && strncmp(rpcmethod, "\"notifications/", 15) == 0)
		http(202, "Accepted", nil, "");
	if(id == nil)
		http(400, "Bad Request", "text/plain", "JSON-RPC request has no id\n");
	if(strcmp(rpcmethod, "\"initialize\"") == 0)
		snprint(replybuf, sizeof replybuf,
			"{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"protocolVersion\":\"2025-03-26\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"plan2001-mcp-test\",\"version\":\"1.0.0\"}}}", id);
	else if(strcmp(rpcmethod, "\"ping\"") == 0)
		snprint(replybuf, sizeof replybuf, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{}}", id);
	else if(strcmp(rpcmethod, "\"tools/list\"") == 0)
		snprint(replybuf, sizeof replybuf,
			"{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"tools\":[{\"name\":\"test_connection\",\"title\":\"Test MCP connection\",\"description\":\"Confirms connectivity with no side effects.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},{\"name\":\"check_c\",\"title\":\"Compile and link C code\",\"description\":\"Compiles and links Plan 9 C source using 6c and 6l in a restricted namespace. It does not execute the resulting program. Returns compiler diagnostics.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"source\":{\"type\":\"string\",\"description\":\"Complete C source code (maximum 16384 characters).\",\"maxLength\":16384}},\"required\":[\"source\"],\"additionalProperties\":false}}]}}", id);
	else if(strcmp(rpcmethod, "\"tools/call\"") == 0){
		tool = value(body, "name");
		if(tool != nil && strcmp(tool, "\"test_connection\"") == 0)
			snprint(replybuf, sizeof replybuf,
				"{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"Connection successful: ChatGPT reached the Plan2001 MCP test server.\"}]}}", id);
		else if(tool != nil && strcmp(tool, "\"check_c\"") == 0){
			n = stringfield(body, "source", source, sizeof source);
			if(n < 0){
				message = strdup(n == -2 ? "Source code exceeds the 16 KiB limit." : "Missing or invalid source string.");
			}else{
				message = checkc(source, n);
			}
			{
				jsontext(quoted, sizeof quoted, message);
				snprint(replybuf, sizeof replybuf,
					"{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}}", id, quoted);
			}
			free(message);
		}else
			snprint(replybuf, sizeof replybuf,
				"{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":\"Unknown test tool.\"}]}}", id);
	}else
		snprint(replybuf, sizeof replybuf,
			"{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}", id);
	free(id);
	free(rpcmethod);
	free(tool);
	http(200, "OK", "application/json", replybuf);
}
