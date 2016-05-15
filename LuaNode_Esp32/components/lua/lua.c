/*
** $Id: lua.c,v 1.160.1.2 2007/12/28 15:32:23 roberto Exp $
** Lua stand-alone interpreter
** See Copyright Notice in lua.h
*/


//#include <signal.h>
#include "c_stdio.h"
#include "c_stdlib.h"
#include "c_string.h"

#define lua_c

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"

//#include "esp_common.h"
#include "uart.h"
#include "esp32/ets_sys.h"
#include "esp_misc.h"

char line_buffer[LUA_MAXINPUT];

//extern uint8 *RcvMsgBuff;
extern uint8 *pRcvMsgBuff;
extern uint8 *pWritePos;
extern uint8 *pReadPos;

lua_State *lua_crtstate;



static lua_State *globalL = NULL;

lua_Load gLoad;


static const char *progname = LUA_PROGNAME;

static void lstop (lua_State *L, lua_Debug *ar) {
  (void)ar;  /* unused arg. */
  lua_sethook(L, NULL, 0, 0);
  luaL_error(L, "interrupted!");
}

static void laction (int i) {
  //signal(i, SIG_DFL); 
  /* if another SIGINT happens before lstop, terminate process (default action) */
  lua_sethook(globalL, lstop, LUA_MASKCALL | LUA_MASKRET | LUA_MASKCOUNT, 1);
}

static void print_usage (void) {
  fprintf(stderr,
  "usage: %s [options] [script [args]].\n"
  "Available options are:\n"
  "  -e stat  execute string " LUA_QL("stat") "\n"
  "  -l name  require library " LUA_QL("name") "\n"
  "  -m limit set memory limit. (units are in Kbytes)\n"
  "  -i       enter interactive mode after executing " LUA_QL("script") "\n"
  "  -v       show version information\n"
  "  --       stop handling options\n"
  "  -        execute stdin and stop handling options\n"
  ,
  progname);
  fflush(stderr);
}


static void l_message (const char *pname, const char *msg) {
  //if (pname) fprintf(stderr, "%s: ", pname);
  //fprintf(stderr, "%s\n", msg);
  //fflush(stderr);
}

static int report (lua_State *L, int status) {
  if (status && !lua_isnil(L, -1)) {
    const char *msg = lua_tostring(L, -1);
    if (msg == NULL) msg = "(error object is not a string)";
    l_message(progname, msg);
    lua_pop(L, 1);
  }
  return status;
}

static int traceback (lua_State *L) {
  if (!lua_isstring(L, 1))  /* 'message' not a string? */
    return 1;  /* keep it intact */
  lua_getfield(L, LUA_GLOBALSINDEX, "debug");
  if (!lua_istable(L, -1) && !lua_isrotable(L, -1)) {
    lua_pop(L, 1);
    return 1;
  }
  lua_getfield(L, -1, "traceback");
  if (!lua_isfunction(L, -1) && !lua_islightfunction(L, -1)) {
    lua_pop(L, 2);
    return 1;
  }
  lua_pushvalue(L, 1);  /* pass error message */
  lua_pushinteger(L, 2);  /* skip this function and traceback */
  lua_call(L, 2, 1);  /* call debug.traceback */
  return 1;
}

static int docall (lua_State *L, int narg, int clear) {
  int status;
  int base = lua_gettop(L) - narg;  /* function index */
  lua_pushcfunction(L, traceback);  /* push traceback function */
  lua_insert(L, base);  /* put it under chunk and args */
  //signal(SIGINT, laction);
  status = lua_pcall(L, narg, (clear ? 0 : LUA_MULTRET), base);
  //signal(SIGINT, SIG_DFL);
  lua_remove(L, base);  /* remove traceback function */
  /* force a complete garbage collection in case of errors */
  if (status != 0) lua_gc(L, LUA_GCCOLLECT, 0);
  return status;
}

#if 0
static int getargs (lua_State *L, char **argv, int n) {
  int narg;
  int i;
  int argc = 0;
  while (argv[argc]) argc++;  /* count total number of arguments */
  narg = argc - (n + 1);  /* number of arguments to the script */
  luaL_checkstack(L, narg + 3, "too many arguments to script");
  for (i=n+1; i < argc; i++)
    lua_pushstring(L, argv[i]);
  lua_createtable(L, narg, n + 1);
  for (i=0; i < argc; i++) {
    lua_pushstring(L, argv[i]);
    lua_rawseti(L, -2, i - n);
  }
  return narg;
}
#endif

static int dofsfile (lua_State *L, const char *name) {
  int status = luaL_loadfsfile(L, name) || docall(L, 0, 1);
  return report(L, status);
}

static int dostring (lua_State *L, const char *s, const char *name) {
  int status = luaL_loadbuffer(L, s, strlen(s), name) || docall(L, 0, 1);
  return report(L, status);
}

static int dolibrary (lua_State *L, const char *name) {
  lua_getglobal(L, "require");
  lua_pushstring(L, name);
  return report(L, docall(L, 1, 1));
}

static const char *get_prompt (lua_State *L, int firstline) {
  const char *p;
  lua_getfield(L, LUA_GLOBALSINDEX, firstline ? "_PROMPT" : "_PROMPT2");
  p = lua_tostring(L, -1);
  if (p == NULL) p = (firstline ? LUA_PROMPT : LUA_PROMPT2);
  lua_pop(L, 1);  /* remove global */
  return p;
}

static int incomplete (lua_State *L, int status) {
  if (status == LUA_ERRSYNTAX) {
    size_t lmsg;
    const char *msg = lua_tolstring(L, -1, &lmsg);
    const char *tp = msg + lmsg - (sizeof(LUA_QL("<eof>")) - 1);
    if (strstr(msg, LUA_QL("<eof>")) == tp) {
      lua_pop(L, 1);
      return 1;
    }
  }
  return 0;  /* else... */
}


/* check that argument has no extra characters at the end */
#define notail(x)	{if ((x)[2] != '\0') return -1;}


static int collectargs (char **argv, int *pi, int *pv, int *pe) {
  int i;
  for (i = 1; argv[i] != NULL; i++) {
    if (argv[i][0] != '-')  /* not an option? */
        return i;
    switch (argv[i][1]) {  /* option */
      case '-':
        notail(argv[i]);
        return (argv[i+1] != NULL ? i+1 : 0);
      case '\0':
        return i;
      case 'i':
        notail(argv[i]);
        *pi = 1;  /* go through */
      case 'v':
        notail(argv[i]);
        *pv = 1;
        break;
      case 'e':
        *pe = 1;  /* go through */
      case 'm':   /* go through */
      case 'l':
        if (argv[i][2] == '\0') {
          i++;
          if (argv[i] == NULL) return -1;
        }
        break;
      default: return -1;  /* invalid option */
    }
  }
  return 0;
}

static int runargs (lua_State *L, char **argv, int n) {
  int i;
  for (i = 1; i < n; i++) {
    if (argv[i] == NULL) continue;
    lua_assert(argv[i][0] == '-');
    switch (argv[i][1]) {  /* option */
      case 'e': {
        const char *chunk = argv[i] + 2;
        if (*chunk == '\0') chunk = argv[++i];
        lua_assert(chunk != NULL);
        if (dostring(L, chunk, "=(command line)") != 0)
          return 1;
        break;
      }
      case 'm': {
        const char *limit = argv[i] + 2;
        int memlimit=0;
        if (*limit == '\0') limit = argv[++i];
        lua_assert(limit != NULL);
        memlimit = atoi(limit);
        lua_gc(L, LUA_GCSETMEMLIMIT, memlimit);
        break;
      }
      case 'l': {
        const char *filename = argv[i] + 2;
        if (*filename == '\0') filename = argv[++i];
        lua_assert(filename != NULL);
        if (dolibrary(L, filename))
          return 1;  /* stop if file fails */
        break;
      }
      default: break;
    }
  }
  return 0;
}

static int handle_luainit (lua_State *L) {
  const char *init = c_getenv(LUA_INIT);
  if (init == NULL) {
	  return 0;  /* status OK */
  }
  else if (init[0] == '@') {
    return dofsfile(L, init+1);
  }
  return dostring(L, init, "=" LUA_INIT);
}


struct Smain {
  int argc;
  char **argv;
  int status;
};

static int pmain (lua_State *L) {
  struct Smain *s = (struct Smain *)lua_touserdata(L, 1);
  char **argv = s->argv;
  int script;
  int has_i = 0, has_v = 0, has_e = 0;
  globalL = L;
  if (argv[0] && argv[0][0]) progname = argv[0];
  lua_gc(L, LUA_GCSTOP, 0);  /* stop collector during initialization */
  luaL_openlibs(L);  /* open libraries */
  lua_gc(L, LUA_GCRESTART, 0);
  s->status = handle_luainit(L);
  script = collectargs(argv, &has_i, &has_v, &has_e);
  if (script < 0) {  /* invalid args? */
    s->status = 1;
    return 0;
  }
  s->status = runargs(L, argv, (script > 0) ? script : s->argc);
  if (s->status != 0) return 0;
  
  return 0;
}

int add(lua_State *L) {
  int a = lua_tointeger(L, 1);
  int b = lua_tointeger(L, 2);
  lua_pushinteger(L, a+b);
  return 1;
}


static void dojob(lua_Load *load);
static bool readline(lua_Load *load);

int lua_main (int argc, char **argv) {
  int status;
  struct Smain s;
  
  lua_State *L = luaL_newstate();
  if (L == NULL) {
    printf("lua failed\n");
    return 1;
  }
  printf("lua create ok!\n");
  
  //luaopen_base(L);

  luaL_openlibs(L);

  /*s.argc = argc;
  s.argv = argv;
  status = lua_cpcall(L, &pmain, &s);
  report(L, status);*/

  gLoad.L = L;
  gLoad.firstline = 1;
  gLoad.done = 0;
  gLoad.line = line_buffer;
  gLoad.len = LUA_MAXINPUT;
  gLoad.line_position = 0;
  gLoad.prmt = get_prompt(L, 1);

  dojob(&gLoad);

  //const char *buff = "local str=222; print(str)";
  //luaL_dostring(L, buff);

  /*const char *buff = "tbl = 25";
  luaL_dostring(L, buff);
  lua_getglobal(L, "tbl");
  int tbl = lua_tointeger(L, -1);
  printf("tbl=%d\n", tbl);*/

  /*const char *buff2 = "function lua_add(a,b) return add(a,b); end";
  lua_register(L, "add", add);
  luaL_dostring(L, buff2);
  lua_getglobal(L, "lua_add");
  lua_pushinteger(L, 6);
  lua_pushinteger(L, 5);
  lua_pcall(L, 2, 1, 0);
  int res = lua_tointeger(L, -1);
  printf("result=%d\n", res);*/

  return 0;
}

void lua_handle_input (bool force)
{
  if (force || readline (&gLoad)) {
    dojob (&gLoad);
  }
}


bool uart_getc(char *c){
    RcvMsgBuff *pRxBuff = &rcvMsgBuff;
    if(pRxBuff->pWritePos == pRxBuff->pReadPos){   // empty
        return false;
    }
    // ETS_UART_INTR_DISABLE();
    ETS_INTR_LOCK();
    *c = (char)*(pRxBuff->pReadPos);
    if (pRxBuff->pReadPos == (pRxBuff->pRcvMsgBuff + RX_BUFF_SIZE)) {
        pRxBuff->pReadPos = pRxBuff->pRcvMsgBuff ; 
    } else {
        pRxBuff->pReadPos++;
    }
    // ETS_UART_INTR_ENABLE();
    ETS_INTR_UNLOCK();
    return true;
}

static void dojob(lua_Load *load) {
  size_t l, rs;
  int status;
  char *b = load->line;
  lua_State *L = load->L;
  
  const char *oldprogname = progname;
  progname = NULL;
  
  do{
    if(load->done == 1){
      l = c_strlen(b);
      if (l > 0 && b[l-1] == '\n')  /* line ends with newline? */
        b[l-1] = '\0';  /* remove it */

	  status = luaL_dostring(L, b);
	  
	  if (status) {
		printf(lua_tostring(L, -1));
		lua_pop(L, 1);
	  }

	  load->firstline = 0;
      load->prmt = get_prompt(L, 1);
    }
  }while(0);
  
  progname = oldprogname;

//  char *b = load->line;
//  lua_State *L = load->L;
//  luaL_dostring(L, b);

  load->done = 0;
  load->line_position = 0;
  memset(load->line, 0, load->len);
  os_printf(load->prmt);
}


static char last_nl_char = '\0';
static bool readline(lua_Load *load){
  int need_dojob = false;
  char ch;
  while (uart_getc(&ch)) {
    char tmp_last_nl_char = last_nl_char;
    // reset marker, will be finally set below when newline is processed
    last_nl_char = '\0';
    
    /* handle CR & LF characters
      filters second char of LF&CR (\n\r) or CR&LF (\r\n) sequences */
    if ((ch == '\r' && tmp_last_nl_char == '\n') || // \n\r sequence -> skip \r
          (ch == '\n' && tmp_last_nl_char == '\r'))   // \r\n sequence -> skip \n
    {
      continue;
    }
      
    if (ch == 0x7f || ch == 0x08) {
      if (load->line_position > 0) {
        uart_tx_one_char(UART0, 0x08);
        uart_tx_one_char(UART0, ' ');
        uart_tx_one_char(UART0, 0x08);
        load->line_position--;
      }
      line_buffer[load->line_position] = 0;
      continue;
    }
    
    /* end of line */
    if (ch == '\r' || ch == '\n') {
      last_nl_char = ch;
      line_buffer[load->line_position] = 0;
	  uart0_putc('\n');
      if (load->line_position == 0){
        /* Get a empty line, then go to get a new line */
		os_printf(load->prmt);
      } else {
		load->done = 1;
        need_dojob = true;
      }
      continue;
    }
    
	uart0_putc(ch);

    /* it's a large line, discard it */
    if ( load->line_position + 1 >= LUA_MAXINPUT ){
      load->line_position = 0;
    }
    
    line_buffer[load->line_position] = ch;
    load->line_position++;
  }
  return need_dojob;
}


void donejob(lua_Load *load){
  lua_close(load->L);
}

void getline(char *src) {
  int len = strlen(src);
  if (len >= LUA_MAXINPUT) {
    printf("The input is too long!\n");
    return;
  }
  gLoad.done = 1;
  memcpy(line_buffer, src, len);
  dojob(&gLoad);
}

