#include <cstring>
#include <cstdio>
#include "plugin_snowball.h"
#include "reader_snowball.h"

#if HAVE_ICU
#include <unicode/uclean.h>
#include <unicode/uversion.h>
#include <unicode/uchar.h>
#include <unicode/unorm.h>
#endif

// mysql headers

#if !defined(__attribute__) && (defined(__cplusplus) || !defined(__GNUC__)  || __GNUC__ == 2 && __GNUC_MINOR__ < 8)
#define __attribute__(A)
#endif

#include <my_sys.h>
static void  icu_free(const void* context, void *ptr){ my_free(ptr,MYF(0)); }
static void* icu_malloc(const void* context, size_t size){ return my_malloc(size,MYF(MY_WME)); }
static void* icu_realloc(const void* context, void* ptr, size_t size){
	if(ptr!=NULL) return my_realloc(ptr,size,MYF(MY_WME));
	return my_malloc(size,MYF(MY_WME));
}

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
	
	UErrorCode ustatus = U_ZERO_ERROR;
	u_setMemoryFunctions(NULL, icu_malloc, icu_realloc, icu_free, &ustatus);
	if(U_FAILURE(ustatus)){
		sprintf(errstr, "u_setMemoryFunctions failed. ICU status code %d\n", ustatus);
		fputs(errstr, stderr);
		fflush(stderr);
	}
#else
	strcat(snowball_info, "without ICU");
#endif
	return 0;
}

static int snowball_parser_plugin_deinit(void *arg __attribute__((unused))){
	return 0;
}


static int snowball_parser_init(MYSQL_FTPARSER_PARAM *param __attribute__((unused))){
	DBUG_ENTER("snowball_parser_init");
	param->ftparser_state = new FtSnowballState(param->cs);
	DBUG_RETURN(0);
}

static int snowball_parser_deinit(MYSQL_FTPARSER_PARAM *param __attribute__((unused))){
	delete (FtSnowballState*)(param->ftparser_state);
	return 0;
}

static void pooled_add_word(FtMemBuffer *membuffer, FtMemPool *pool, MYSQL_FTPARSER_PARAM *param, MYSQL_FTPARSER_BOOLEAN_INFO *info){
	size_t length;
	size_t capacity;
	char *binary = membuffer->getBuffer(&length, &capacity);
	
// 	char b[1024];
// 	memcpy(b,binary,length);
// 	b[length]='\0';
// 	fprintf(stderr, "add %s\n", b); fflush(stderr);
	
	if(length > 0){
		info->type = FT_TOKEN_WORD;
		const char *save = pool->findPool(param->doc, param->length, (const char*)binary, length);
		if(save){
			if(info){ param->mysql_add_word(param, (char*)save, length, info); }
		}else{
			membuffer->detach();
			if(info){ param->mysql_add_word(param, binary, length, info); }
			pool->addHeap(new FtMemHeap(binary, length, capacity));
		}
	}
}

static int snowball_parser_parse(MYSQL_FTPARSER_PARAM *param){
	DBUG_ENTER("snowball_parser_parse");
	FtSnowballState *state = (FtSnowballState*)(param->ftparser_state);
	FtCharReader *reader;
	FtMemReader memReader(const_cast<char*>(param->doc), (size_t)param->length, param->cs);
	reader = &memReader;
	
	MYSQL_FTPARSER_BOOLEAN_INFO info = { FT_TOKEN_WORD, 1, 0, 0, 0, ' ', 0 };
	
	if(param->mode == MYSQL_FTPARSER_FULL_BOOLEAN_INFO){
		FtBoolReader boolParser(reader);
		reader = &boolParser;
		
		FtCharReader *breaker = NULL;
		if(!snowball_breaker || strcmp(snowball_breaker,"DEFAULT")==0){
			breaker = new FtBreakReader(reader, param->cs);
		}else{
#if HAVE_ICU
			breaker = new FtUnicodeBreakReader(reader, snowball_breaker);
#endif
		}
		if(breaker){
			reader = breaker;
		}
		
		FtSnowballReader snowball(reader, state->engine, state->engine_charset);
		reader = &snowball;
	
#if HAVE_ICU
		// post-normalizer
		FtUnicodeNormalizerReader *normReader = NULL;
		if(state->normalization != FT_NORM_OFF){
			if(state->normalization == FT_NORM_NFC){
				normReader = new FtUnicodeNormalizerReader(reader, UNORM_NFC);
			}else if(state->normalization == FT_NORM_NFD){
				normReader = new FtUnicodeNormalizerReader(reader, UNORM_NFD);
			}else if(state->normalization == FT_NORM_NFKC){
				normReader = new FtUnicodeNormalizerReader(reader, UNORM_NFKC);
			}else if(state->normalization == FT_NORM_NFKD){
				normReader = new FtUnicodeNormalizerReader(reader, UNORM_NFKD);
			}else if(state->normalization == FT_NORM_FCD){
				normReader = new FtUnicodeNormalizerReader(reader, UNORM_FCD);
			}
			if(state->unicode_v32){
				normReader->setOption(UNORM_UNICODE_3_2, TRUE);
			}
			reader = normReader;
		}
#endif
		
		FtMemBuffer memBuffer(param->cs);
		
		char dummy = '\"';
		my_wc_t wc;
		int meta;
		while(reader->readOne(&wc, &meta)){
			if(meta==FT_CHAR_NORM){
				memBuffer.append(wc);
			}else{
				memBuffer.flush();
				pooled_add_word(&memBuffer, state->pool, param, &info);
				memBuffer.reset();
				
				if(meta==FT_CHAR_CTRL){       info.yesno = 0; }
				else if(meta&FT_CHAR_YES){    info.yesno = +1; }
				else if(meta&FT_CHAR_NO){     info.yesno = -1; }
				else if(meta&FT_CHAR_STRONG){ info.weight_adjust++; }
				else if(meta&FT_CHAR_WEAK){   info.weight_adjust--; }
				else if(meta&FT_CHAR_NEG){    info.wasign = !info.wasign; }
				else if(meta&FT_CHAR_TRUNC){  info.trunc = 1; }
				
				if(meta&FT_CHAR_LEFT){
					info.type = FT_TOKEN_LEFT_PAREN;
					if(meta&FT_CHAR_QUOT){
						info.quot = &dummy;
					}
					param->mysql_add_word(param, NULL, 0, &info);
					info.type = FT_TOKEN_WORD;
				}else if(meta&FT_CHAR_RIGHT){
					info.type = FT_TOKEN_RIGHT_PAREN;
					param->mysql_add_word(param, NULL, 0, &info);
					if(meta&FT_CHAR_QUOT){
						info.quot = NULL;
					}
					info.type = FT_TOKEN_WORD;
				}
			}
		}
		memBuffer.flush();
		pooled_add_word(&memBuffer, state->pool, param, &info);
#if HAVE_ICU
		if(normReader){ delete normReader; }
#endif
		if(breaker){ delete breaker; }
	}else{
		FtCharReader *breaker = NULL;
		if(!snowball_breaker || strcmp(snowball_breaker,"DEFAULT")==0){
			breaker = new FtBreakReader(reader, param->cs);
		}else{
#if HAVE_ICU
			breaker = new FtUnicodeBreakReader(reader, snowball_breaker);
#endif
		}
		if(breaker){
			reader = breaker;
		}
		
		FtSnowballReader snowball(reader, state->engine, state->engine_charset);
		reader = &snowball;
	
#if HAVE_ICU
		// post-normalizer
		FtUnicodeNormalizerReader *normReader = NULL;
		if(state->normalization != FT_NORM_OFF){
			if(state->normalization == FT_NORM_NFC){
				normReader = new FtUnicodeNormalizerReader(reader, UNORM_NFC);
			}else if(state->normalization == FT_NORM_NFD){
				normReader = new FtUnicodeNormalizerReader(reader, UNORM_NFD);
			}else if(state->normalization == FT_NORM_NFKC){
				normReader = new FtUnicodeNormalizerReader(reader, UNORM_NFKC);
			}else if(state->normalization == FT_NORM_NFKD){
				normReader = new FtUnicodeNormalizerReader(reader, UNORM_NFKD);
			}else if(state->normalization == FT_NORM_FCD){
				normReader = new FtUnicodeNormalizerReader(reader, UNORM_FCD);
			}
			if(state->unicode_v32){
				normReader->setOption(UNORM_UNICODE_3_2, TRUE);
			}
			reader = normReader;
		}
#endif
		
		FtMemBuffer memBuffer(param->cs);
		
		my_wc_t wc;
		int meta;
		while(reader->readOne(&wc, &meta)){
//			fprintf(stderr,"plugin_got %lu %d\n", wc, meta); fflush(stderr);
			if(meta==FT_CHAR_NORM){
				memBuffer.append(wc);
			}else{
				memBuffer.flush();
				pooled_add_word(&memBuffer, state->pool, param, &info);
				memBuffer.reset();
			}
		}
		memBuffer.flush();
		pooled_add_word(&memBuffer, state->pool, param, &info);
#if HAVE_ICU
		if(normReader){ delete normReader; }
#endif
		if(breaker){ delete breaker; }
	}
	DBUG_RETURN(0);
}

int snowball_breaker_check(MYSQL_THD thd, struct st_mysql_sys_var *var, void *save, struct st_mysql_value *value){
	char buf[64];
	int len=64;
	const char *str;
	
	str = value->val_str(value,buf,&len);
	if(!str) return -1;
	*(const char**)save=str;
	
	if(strcmp(str,"DEFAULT")==0){
		return 0;
	}else{
#if HAVE_ICU
		BreakIterator *breaker = NULL;
		
		UErrorCode status = U_ZERO_ERROR;
		int32_t count = 0;
		const Locale *locales = Locale::getAvailableLocales(count);
		int32_t i=0;
		for(i=0; i<count; i++){
			if(strcasecmp(locales[i].getName(), str)==0){
				breaker = BreakIterator::createWordInstance(locales[i], status);
				break;
			}
		}
		if(breaker){
			delete breaker;
			if(status==U_USING_DEFAULT_WARNING || status==U_ZERO_ERROR){
				return 0;
			}
		}
#endif
	}
	return -1;
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

int snowball_normalization_check(MYSQL_THD thd, struct st_mysql_sys_var *var, void *save, struct st_mysql_value *value){
	char buf[4];
	int len=4;
	const char *str;

	str = value->val_str(value,buf,&len);
	if(!str) return -1;
	*(const char**)save=str;
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


FtSnowballState::FtSnowballState(CHARSET_INFO *cs){
	pool = new FtMemPool();
	
	const char *algorithm = "english";
	if(snowball_algorithm && strlen(snowball_algorithm) > 0){
		algorithm = snowball_algorithm;
	}
	engine = sb_stemmer_new(algorithm, "UTF_8");
	engine_charset = get_charset(33, MYF(0)); // utf8_general_ci
	if(!engine || !engine_charset){
		if(strcmp(cs->csname, "latin1")==0){
			engine = sb_stemmer_new(algorithm, "ISO_8859_1");
			engine_charset = get_charset(8, MYF(0));  // latin1_swedish_ci
		}else if(strcmp(cs->csname, "latin2")==0){
			engine = sb_stemmer_new(algorithm, "ISO_8859_2");
			engine_charset = get_charset(9, MYF(0));  // latin2_general_ci
		}else if(strcmp(cs->csname, "koi8r")==0){
			engine = sb_stemmer_new(algorithm, "KOI8_R");
			engine_charset = get_charset(7, MYF(0));  // koi8r_general_ci
		}
	}
	// XXX: if engine_charset==NULL, ICU converter might be the alternative code page converter.
	
	normalization = FT_NORM_OFF;
	unicode_v32 = false;
}

FtSnowballState::~FtSnowballState(){
	delete pool;
	if(engine){
		sb_stemmer_delete(engine);
	}
}
