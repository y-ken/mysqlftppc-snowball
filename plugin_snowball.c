#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "ftbool.h"
#include "ftstring.h"
#if HAVE_ICU
#include <unicode/uclean.h>
#include <unicode/uversion.h>
#include <unicode/uchar.h>
#include <unicode/unorm.h>
#include "ftnorm.h"
#endif
#include "libstemmer_c/include/libstemmer.h"

// mysql headers
#include <my_global.h>
#include <m_ctype.h>
#include <my_sys.h>
#include <my_list.h>
#include <plugin.h>

#define HA_FT_MAXBYTELEN 254
#define FTPPC_MEMORY_ERROR -1
#define FTPPC_NORMALIZATION_ERROR -2
#define FTPPC_SYNTAX_ERROR -3

#if !defined(__attribute__) && (defined(__cplusplus) || !defined(__GNUC__)  || __GNUC__ == 2 && __GNUC_MINOR__ < 8)
#define __attribute__(A)
#endif

static char* snowball_unicode_normalize;
static char* snowball_unicode_version;
static char* snowball_algorithm;
static char snowball_info[128];

static void  icu_free(const void* context, void *ptr){ my_free(ptr); }
static void* icu_malloc(const void* context, size_t size){ return my_malloc(size,MYF(MY_WME)); }
static void* icu_realloc(const void* context, void* ptr, size_t size){
  if(ptr!=NULL) return my_realloc(ptr,size,MYF(MY_WME));
  else return my_malloc(size,MYF(MY_WME));
}


/** ftstate */
static LIST* list_top(LIST* root){
  LIST *cur = root;
  while(cur && cur->next){
    cur = cur->next;
  }
  return cur;
}

struct ftppc_mem_bulk {
  char*  mem_head;
  char*  mem_cur;
  size_t mem_size;
};

struct ftppc_state {
  /** immutable memory buffer */
  size_t bulksize;
  LIST*  mem_root;
  void*  engine;
  CHARSET_INFO* engine_charset;
};

static void* ftppc_alloc(struct ftppc_state *state, size_t length){
  LIST *cur = list_top(state->mem_root);
  while(1){
    if(!cur){
      // hit the root. create a new bulk.
      size_t sz = state->bulksize<<1;
      while(sz < length){
        sz = sz<<1;
      }
      state->bulksize = sz;
      
      struct ftppc_mem_bulk *tmp = (struct ftppc_mem_bulk*)my_malloc(sizeof(struct ftppc_mem_bulk), MYF(MY_WME));
      tmp->mem_head = my_malloc(sz, MYF(MY_WME));
      tmp->mem_cur  = tmp->mem_head;
      tmp->mem_size = sz;
      
      if(!tmp->mem_head){ return NULL; }
      
      state->mem_root = list_cons(tmp, cur);
      cur = state->mem_root;
    }
    
    struct ftppc_mem_bulk *bulk = (struct ftppc_mem_bulk*)cur->data;
    
    if(bulk->mem_cur + length < bulk->mem_head + bulk->mem_size){
      void* addr = bulk->mem_cur;
      bulk->mem_cur += length;
      return addr;
    }
    cur = cur->prev;
  }
}
/** /ftstate */


static int snowball_parser_plugin_init(void *arg __attribute__((unused))){
  snowball_info[0] = '\0';
#if HAVE_ICU
  char icu_tmp_str[16];
  char errstr[128];
  UVersionInfo versionInfo;
  u_getVersion(versionInfo); // get ICU version
  u_versionToString(versionInfo, icu_tmp_str);
  strcat(snowball_info, "with ICU ");
  strcat(snowball_info, icu_tmp_str);
  u_getUnicodeVersion(versionInfo); // get ICU Unicode version
  u_versionToString(versionInfo, icu_tmp_str);
  strcat(snowball_info, "(Unicode ");
  strcat(snowball_info, icu_tmp_str);
  strcat(snowball_info, ")");
  
  UErrorCode ustatus=0;
  u_setMemoryFunctions(NULL, icu_malloc, icu_realloc, icu_free, &ustatus);
  if(U_FAILURE(ustatus)){
    sprintf(errstr, "u_setMemoryFunctions failed. ICU status code %d\n", ustatus);
    fputs(errstr, stderr);
    fflush(stderr);
  }
#else
  strcat(snowball_info, "without ICU");
#endif
  return(0);
}

static int snowball_parser_plugin_deinit(void *arg __attribute__((unused))){
  return(0);
}


static int snowball_parser_init(MYSQL_FTPARSER_PARAM *param __attribute__((unused))){
  struct ftppc_state tmp ={ 8, NULL, NULL, NULL };
  struct ftppc_state *state = (struct ftppc_state*)my_malloc(sizeof(struct ftppc_state), MYF(MY_WME));
  if(!state){
    return(FTPPC_MEMORY_ERROR);
  }
  *state = tmp;
  param->ftparser_state = state;
  
  return(0);
}
static int snowball_parser_deinit(MYSQL_FTPARSER_PARAM *param __attribute__((unused))){
  struct ftppc_state *state = (struct ftppc_state*)param->ftparser_state;
  sb_stemmer_delete((struct sb_stemmer *)state->engine);
  list_free(state->mem_root, 1);
  my_free(state);
  return(0);
}

static int iswchar(CHARSET_INFO *cs, char *doc, char *end, int *readsize){
  my_wc_t wc;
  int ctype;
  *readsize = cs->cset->ctype(cs, &ctype, (uchar*)doc, (uchar*)end);
  if(ctype & (_MY_U | _MY_L | _MY_NMR)){
    return 1;
  }
  cs->cset->mb_wc(cs, &wc, (uchar*)doc, (uchar*)end);
  if(wc == '_'){ 
    return 1;
  }
  return 0;
}

static size_t str_convert(CHARSET_INFO *cs, char *from, size_t from_length,
                          CHARSET_INFO *uc, char *to,   size_t to_length,
                          size_t *numchars){
  char *rpos, *rend, *wpos, *wend;
  my_wc_t wc;
  char* tmp = NULL;
  
  if(numchars){ *numchars = 0; }
  rpos = from;
  rend = from + from_length;
  wpos = to;
  wend = to + to_length;
  while(rpos < rend){
    int cnvres = 0;
    cnvres = cs->cset->mb_wc(cs, &wc, (uchar*)rpos, (uchar*)rend);
    if(cnvres > 0){
      rpos += cnvres;
    }else if(cnvres == MY_CS_ILSEQ){
      rpos++;
      wc = '?';
    }else{
      break;
    }
    if(!to){
      if(!tmp){ tmp=my_malloc(uc->mbmaxlen, MYF(MY_WME)); }
      cnvres = uc->cset->wc_mb(uc, wc, (uchar*)tmp, (uchar*)(tmp+uc->mbmaxlen));
    }else{
      cnvres = uc->cset->wc_mb(uc, wc, (uchar*)wpos, (uchar*)wend);
    }
    if(cnvres > 0){
      wpos += (size_t)cnvres;
    }else{
      break;
    }
    if(numchars){ *numchars++; }
  }
  if(tmp){ my_free(tmp); }
  return (size_t)(wpos-to);
}

static int snowball_add_word(MYSQL_FTPARSER_PARAM *param, FTSTRING *pbuffer, MYSQL_FTPARSER_BOOLEAN_INFO* instinfo){
  size_t tlen = ftstring_length(pbuffer);
  if(tlen==0){
    return 0;
  }
  char* thead = ftstring_head(pbuffer);
  
  struct ftppc_state *state = (struct ftppc_state*)param->ftparser_state;
  
  struct sb_stemmer *st = (struct sb_stemmer*)(state->engine);
  const sb_symbol * sym = sb_stemmer_stem(st, (const sb_symbol*)thead, tlen);
  int sym_len = sb_stemmer_length(st);
  
  if(!sym){
    // stemming failed.
    return 0;
  }
  
  if(strcmp(state->engine_charset->csname, param->cs->csname) != 0){
    tlen = str_convert(state->engine_charset, (char*)sym, (size_t)sym_len, param->cs, NULL, 0, NULL);
    thead = ftppc_alloc(state, tlen);
    if(!thead){
      return FTPPC_MEMORY_ERROR;
    }
    str_convert(state->engine_charset, (char*)sym, (size_t)sym_len, param->cs, thead, tlen, NULL);
  }else{
    tlen = (size_t)sym_len;
    thead = ftppc_alloc(state, tlen);
    if(!thead){
      return FTPPC_MEMORY_ERROR;
    }
    memcpy(thead, (char*)sym, tlen);
  }
  return param->mysql_parse(param, thead, tlen);
}

static int snowball_parser_parse(MYSQL_FTPARSER_PARAM *param)
{
  char* feed = param->doc;
  size_t feed_length = (size_t)param->length;
  int feed_req_free = 0;
  CHARSET_INFO *cs = param->cs; // the charset of feed
  
#if HAVE_ICU
  // normalize.
  if(snowball_unicode_normalize && strcmp(snowball_unicode_normalize, "OFF")!=0){
    if(strcmp(cs->csname, "utf8")!=0){
      // convert into UTF-8
      CHARSET_INFO *uc = get_charset(33, MYF(0)); // my_charset_utf8_general_ci for utf8 conversion
      // calculate mblen and malloc.
//      size_t cv_length = uc->mbmaxlen * cs->cset->numchars(cs, feed, feed+feed_length);
      size_t cv_length = str_convert(cs, feed, feed_length, uc, NULL, 0, NULL);
      char* cv = my_malloc(cv_length, MYF(MY_WME));
      feed_length = str_convert(cs, feed, feed_length, uc, cv, cv_length, NULL);
      feed = cv;
      feed_req_free = 1;
      cs = uc;
    }
    char* nm;
    char* t;
    size_t nm_length=0;
    size_t nm_used=0;
    nm_length = feed_length+32;
    nm = my_malloc(nm_length, MYF(MY_WME));
    int status = 0;
    int mode = UNORM_NONE;
    int options = 0;
    if(strcmp(snowball_unicode_normalize, "C")==0) mode = UNORM_NFC;
    if(strcmp(snowball_unicode_normalize, "D")==0) mode = UNORM_NFD;
    if(strcmp(snowball_unicode_normalize, "KC")==0) mode = UNORM_NFKC;
    if(strcmp(snowball_unicode_normalize, "KD")==0) mode = UNORM_NFKD;
    if(strcmp(snowball_unicode_normalize, "FCD")==0) mode = UNORM_FCD;
    if(snowball_unicode_version && strcmp(snowball_unicode_version, "3.2")==0) options |= UNORM_UNICODE_3_2;
    if(feed_length > 0){
      nm_used = uni_normalize(feed, feed_length, nm, nm_length, mode, options);
      if(nm_used == 0){
        fputs("unicode normalization failed.\n",stderr);
        fflush(stderr);
        
        if(feed_req_free){ my_free(feed); }
        return FTPPC_NORMALIZATION_ERROR;
      }else if(nm_used > nm_length){
        nm_length = nm_used + 8;
        char *tmp = my_realloc(nm, nm_length, MYF(MY_WME));
        if(tmp){
          nm = tmp;
        }else{
          if(feed_req_free){ my_free(feed); }
          my_free(nm);
          return FTPPC_MEMORY_ERROR;
        }
        nm_used = uni_normalize(feed, feed_length, nm, nm_length, mode, options);
        if(nm_used == 0){
          fputs("unicode normalization failed.\n",stderr);
          fflush(stderr);
          
          if(feed_req_free){ my_free(feed); }
          my_free(nm);
          return FTPPC_NORMALIZATION_ERROR;
        }
      }
      if(feed_req_free){ my_free(feed); }
      feed = nm;
      feed_length = nm_used;
      feed_req_free = 1;
    }
  }
#endif
  
  struct ftppc_state *state = (struct ftppc_state*)param->ftparser_state;
  // init engine >>
  if(!state->engine){
    char *algorithm = "english";
    if(snowball_algorithm && strlen(snowball_algorithm) > 0){
      algorithm = snowball_algorithm;
    }
    
    struct sb_stemmer *st = NULL;
    CHARSET_INFO *cs;
    if(strcmp(param->cs->csname, "utf8")==0){
      cs = get_charset(83, MYF(0)); // cs (utf8_bin) can't be null.
      st = sb_stemmer_new(algorithm, "UTF_8");
      if(st){
        state->engine = st;
        state->engine_charset = cs;
      }
    }else if(strcmp(param->cs->csname, "latin1")==0){
      cs = get_charset(47, MYF(0)); // cs (latin1_bin) can't be null.
      st = sb_stemmer_new(algorithm, "ISO_8858_1");
      if(st){
        state->engine = st;
        state->engine_charset = cs;
      }
    }else if(strcmp(param->cs->csname, "cp850")==0){
      cs = get_charset(80, MYF(0)); // cs (cp850_bin) can't be null.
      st = sb_stemmer_new(algorithm, "CP850");
      if(st){
        state->engine = st;
        state->engine_charset = cs;
      }
    }else if(strcmp(param->cs->csname, "koi8r")==0){
      cs = get_charset(74, MYF(0)); // cs (koi8r_bin) can't be null.
      st = sb_stemmer_new(algorithm, "KOI8_R");
      if(st){
        state->engine = st;
        state->engine_charset = cs;
      }
    }
    if(!st){ // if engine was not available, we'll transcode.
      cs = get_charset(83, MYF(0)); // cs (utf8_bin) can't be null.
      st = sb_stemmer_new(algorithm, "UTF_8"); // st can't be NULL because update function will check it.
      state->engine = st;
      state->engine_charset = cs;
    }
  }
  // init engine <<
  if(strcmp(cs->csname, state->engine_charset->csname)!=0){
    size_t tmp_len = str_convert(cs, feed, feed_length, state->engine_charset, NULL, 0, NULL);
    char* tmp = my_malloc(tmp_len, MYF(MY_WME));
    if(!tmp){
      return FTPPC_MEMORY_ERROR;
    }
    str_convert(cs, feed, feed_length, state->engine_charset, tmp, tmp_len, NULL);
    if(feed_req_free){ my_free(feed); }
    feed = tmp;
    feed_length = tmp_len;
    feed_req_free = 1;
    cs = state->engine_charset;
  }
  
  FTSTRING buffer = { NULL, 0, NULL, 0, 0 };
  FTSTRING *pbuffer = &buffer;
  ftstring_bind(pbuffer, feed, feed_req_free);
  
  if(param->mode == MYSQL_FTPARSER_FULL_BOOLEAN_INFO){
    MYSQL_FTPARSER_BOOLEAN_INFO instinfo ={ FT_TOKEN_WORD, 0, 0, 0, 0, ' ', 0 };
    MYSQL_FTPARSER_BOOLEAN_INFO *info_may = (MYSQL_FTPARSER_BOOLEAN_INFO*)my_malloc(sizeof(MYSQL_FTPARSER_BOOLEAN_INFO), MYF(MY_WME));
    if(!info_may){
      return FTPPC_MEMORY_ERROR;
    }
    *info_may = instinfo;
    LIST *infos = NULL;
    list_push(infos, info_may);
    
    int context=CTX_CONTROL;
    SEQFLOW sf,sf_prev = SF_BROKEN;
    char* pos = feed;
    char* docend = feed+feed_length;
    while(pos < docend){
      int readsize;
      my_wc_t dst;
      sf = ctxscan(cs, pos, docend, &dst, &readsize, context);
      if(sf==SF_ESCAPE){
        context |= CTX_ESCAPE;
        context |= CTX_CONTROL;
      }else{
        context &= ~CTX_ESCAPE;
        if(sf == SF_CHAR){
          context &= ~CTX_CONTROL;
        }else{
          context |= CTX_CONTROL;
        }
      }
      if(context & CTX_QUOTE){
        if(my_isspace(param->cs, *pos) && sf_prev!=SF_ESCAPE){ // perform phrase query.
          sf = SF_WHITE;
        }
      }
      if(sf == SF_PLUS){   instinfo.yesno = 1; }
      if(sf == SF_MINUS){  instinfo.yesno = -1; }
      if(sf == SF_STRONG){ instinfo.weight_adjust++; }
      if(sf == SF_WEAK){   instinfo.weight_adjust--; }
      if(sf == SF_WASIGN){ instinfo.wasign = !instinfo.wasign; }
      if(sf == SF_LEFT_PAREN){
        MYSQL_FTPARSER_BOOLEAN_INFO *tmp = (MYSQL_FTPARSER_BOOLEAN_INFO*)my_malloc(sizeof(MYSQL_FTPARSER_BOOLEAN_INFO), MYF(MY_WME));
        if(!tmp){
          list_free(infos, 1);
          ftstring_destroy(pbuffer);
          return FTPPC_MEMORY_ERROR;
        }
        *tmp = instinfo;
        list_push(infos, tmp);
        
        instinfo.type = FT_TOKEN_LEFT_PAREN;
        param->mysql_add_word(param, pos, 0, &instinfo); // push LEFT_PAREN token
        instinfo = *tmp;
      }
      if(sf == SF_QUOTE_START){
        context |= CTX_QUOTE;
      }
      if(sf == SF_RIGHT_PAREN){
        instinfo = *((MYSQL_FTPARSER_BOOLEAN_INFO*)infos->data);
        instinfo.type = FT_TOKEN_RIGHT_PAREN;
        param->mysql_add_word(param, pos, 0, &instinfo); // push RIGHT_PAREN token
        
        MYSQL_FTPARSER_BOOLEAN_INFO *tmp = (MYSQL_FTPARSER_BOOLEAN_INFO*)infos->data;
        if(tmp){ my_free(tmp); }
        list_pop(infos);
        if(!infos){
          return FTPPC_SYNTAX_ERROR;
        } // must not reach the base info_may level.
        instinfo = *((MYSQL_FTPARSER_BOOLEAN_INFO*)infos->data);
      }
      if(sf == SF_QUOTE_END){
        context &= ~CTX_QUOTE;
      }
      if(sf == SF_CHAR){
        if(ftstring_length(pbuffer)==0){
          ftstring_bind(pbuffer, pos, feed_req_free);
        }
        ftstring_append(pbuffer, pos, readsize);
      }else if(sf != SF_ESCAPE){
        if(sf == SF_TRUNC){
          instinfo.trunc = 1;
        }
        snowball_add_word(param, pbuffer, &instinfo);
        ftstring_reset(pbuffer);
        instinfo = *((MYSQL_FTPARSER_BOOLEAN_INFO *)infos->data);
      }
      
      if(readsize > 0){
        pos += readsize;
      }else if(readsize == MY_CS_ILSEQ){
        pos++;
      }else{
        break;
      }
      sf_prev = sf;
    }
    if(sf==SF_CHAR){
      if(ftstring_length(pbuffer)>0){ // we must not exceed HA_FT_MAXBYTELEN-HA_FT_WLEN
        snowball_add_word(param, pbuffer, &instinfo);
      }
    }
    list_free(infos,1);
  }else{
    char *s = feed;
    char *e = feed + feed_length;
    while(s < e){
      int readsize;
      if(iswchar(cs, s, e, &readsize)){
        if(ftstring_length(pbuffer) == 0){
          ftstring_bind(pbuffer, s, feed_req_free);
        }
        ftstring_append(pbuffer, s, readsize);
      }else{
        if(ftstring_length(pbuffer) > 0){
          snowball_add_word(param, pbuffer, NULL);
          ftstring_reset(pbuffer);
        }
      }
      s += readsize;
    }
  }
  if(ftstring_length(pbuffer) > 0){
    snowball_add_word(param, pbuffer, NULL);
  }
  ftstring_destroy(pbuffer);
  if(feed_req_free){ my_free(feed); }
  return 0;
}

int snowball_algorithm_check(MYSQL_THD thd, struct st_mysql_sys_var *var, void *save, struct st_mysql_value *value){
    char buf[4];
    int len=4;
    const char *str;
    
    str = value->val_str(value,buf,&len);
    if(!str) return -1;
    *(const char**)save=str;
    
    // we want to use alias names, we don't use sb_stemmer_list().
    struct sb_stemmer *st = sb_stemmer_new(str, NULL);
    if(st){
      sb_stemmer_delete(st);
      return 0;
    }
    return -1;
}

int snowball_unicode_version_check(MYSQL_THD thd, struct st_mysql_sys_var *var, void *save, struct st_mysql_value *value){
    char buf[4];
    int len=4;
    const char *str;
    
    str = value->val_str(value,buf,&len);
    if(!str) return -1;
    *(const char**)save=str;
    if(len==3){
      if(memcmp(str, "3.2", len)==0) return 0;
    }
    if(len==7){
      if(memcmp(str, "DEFAULT", len)==0) return 0;
    }
    return -1;
}

int snowball_unicode_normalize_check(MYSQL_THD thd, struct st_mysql_sys_var *var, void *save, struct st_mysql_value *value){
    char buf[4];
    int len=4;
    const char *str;
    
    str = value->val_str(value,buf,&len);
    if(!str) return -1;
    *(const char**)save=str;
    if(!get_charset(33, MYF(0))) return -1; // If you don't have utf8 codec in mysql, it fails
    if(len==1){
        if(str[0]=='C'){ return 0;}
        if(str[0]=='D'){ return 0;}
    }
    if(len==2){
        if(str[0]=='K' && str[1]=='C'){ return 0;}
        if(str[0]=='K' && str[1]=='D'){ return 0;}
    }
    if(len==3){
        if(str[0]=='F' && str[1]=='C' && str[2]=='D'){ return 0;}
        if(str[0]=='O' && str[1]=='F' && str[2]=='F'){ return 0;}
    }
    return -1;
}

static MYSQL_SYSVAR_STR(algorithm, snowball_algorithm,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
  "Set stemming algorithm by ISO 639 codes.",
  snowball_algorithm_check, NULL, "english");

static MYSQL_SYSVAR_STR(normalization, snowball_unicode_normalize,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
  "Set unicode normalization (OFF, C, D, KC, KD, FCD)",
  snowball_unicode_normalize_check, NULL, "OFF");

static MYSQL_SYSVAR_STR(unicode_version, snowball_unicode_version,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
  "Set unicode version (3.2, DEFAULT)",
  snowball_unicode_version_check, NULL, "DEFAULT");

static struct st_mysql_show_var snowball_status[]=
{
  {"Snowball_info", (char *)snowball_info, SHOW_CHAR},
  {0,0,0}
};

static struct st_mysql_sys_var* snowball_system_variables[]= {
  MYSQL_SYSVAR(algorithm),
#if HAVE_ICU
  MYSQL_SYSVAR(normalization),
  MYSQL_SYSVAR(unicode_version),
#endif
  NULL
};

static struct st_mysql_ftparser snowball_parser_descriptor=
{
  MYSQL_FTPARSER_INTERFACE_VERSION, /* interface version      */
  snowball_parser_parse,              /* parsing function       */
  snowball_parser_init,               /* parser init function   */
  snowball_parser_deinit              /* parser deinit function */
};

mysql_declare_plugin(ft_snowball)
{
  MYSQL_FTPARSER_PLUGIN,      /* type                            */
  &snowball_parser_descriptor,  /* descriptor                      */
  "snowball",                   /* name                            */
  "Hiroaki Kawai",            /* author                          */
  "snowball Full-Text Parser", /* description                     */
  PLUGIN_LICENSE_BSD,
  snowball_parser_plugin_init,  /* init function (when loaded)     */
  snowball_parser_plugin_deinit,/* deinit function (when unloaded) */
  0x0106,                     /* version                         */
  snowball_status,               /* status variables                */
  snowball_system_variables,     /* system variables                */
  NULL
}
mysql_declare_plugin_end;

