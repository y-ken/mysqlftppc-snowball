#ifdef __cplusplus
extern "C" {
#endif

#include <plugin.h>
static char* snowball_breaker;
static char* snowball_normalization;
static char* snowball_unicode_version;
static char* snowball_algorithm;
static char  snowball_info[128];

static int snowball_breaker_check(MYSQL_THD thd, struct st_mysql_sys_var *var, void *save, struct st_mysql_value *value);
static int snowball_algorithm_check(MYSQL_THD thd, struct st_mysql_sys_var *var, void *save, struct st_mysql_value *value);
static int snowball_normalization_check(MYSQL_THD thd, struct st_mysql_sys_var *var, void *save, struct st_mysql_value *value);
static int snowball_unicode_version_check(MYSQL_THD thd, struct st_mysql_sys_var *var, void *save, struct st_mysql_value *value);

static int snowball_parser_plugin_init(void *arg);
static int snowball_parser_plugin_deinit(void *arg);
static int snowball_parser_init(MYSQL_FTPARSER_PARAM *param);
static int snowball_parser_deinit(MYSQL_FTPARSER_PARAM *param);
static int snowball_parser_parse(MYSQL_FTPARSER_PARAM *param);

static MYSQL_SYSVAR_STR(breaker, snowball_breaker,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
  "Set breaking rule. (DEFAULT or en_US, etc.)",
  snowball_breaker_check, NULL, "DEFAULT");

static MYSQL_SYSVAR_STR(algorithm, snowball_algorithm,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
  "Set stemming algorithm by ISO 639 codes.",
  snowball_algorithm_check, NULL, "english");

static MYSQL_SYSVAR_STR(normalization, snowball_normalization,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
  "Set unicode normalization (OFF, C, D, KC, KD, FCD)",
  snowball_normalization_check, NULL, "OFF");

static MYSQL_SYSVAR_STR(unicode_version, snowball_unicode_version,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
  "Set unicode version (3.2, DEFAULT)",
  snowball_unicode_version_check, NULL, "DEFAULT");

static struct st_mysql_sys_var* snowball_system_variables[] = {
  MYSQL_SYSVAR(algorithm),
  MYSQL_SYSVAR(breaker),
  MYSQL_SYSVAR(normalization),
#if HAVE_ICU
  MYSQL_SYSVAR(unicode_version),
#endif
  NULL
};


static struct st_mysql_show_var snowball_status[] = {
  {"Snowball_info", (char *)snowball_info, SHOW_CHAR},
  {NULL,NULL,SHOW_UNDEF}
};

static struct st_mysql_ftparser snowball_parser_descriptor = {
	MYSQL_FTPARSER_INTERFACE_VERSION, /* interface version      */
	snowball_parser_parse,              /* parsing function       */
	snowball_parser_init,               /* parser init function   */
	snowball_parser_deinit              /* parser deinit function */
};

mysql_declare_plugin(ft_snowball)
{
	MYSQL_FTPARSER_PLUGIN,        /* type                            */
	&snowball_parser_descriptor,  /* descriptor                      */
	"snowball",                   /* name                            */
	"Hiroaki Kawai",              /* author                          */
	"snowball Full-Text Parser",  /* description                     */
	PLUGIN_LICENSE_BSD,
	snowball_parser_plugin_init,  /* init function (when loaded)     */
	snowball_parser_plugin_deinit,/* deinit function (when unloaded) */
	0x0200,                       /* version                         */
	snowball_status,              /* status variables                */
	snowball_system_variables,    /* system variables                */
	NULL
}
mysql_declare_plugin_end;

#ifdef __cplusplus
}

#include "mempool.h"
#include "libstemmer_c/include/libstemmer.h"
#include "reader_norm.h"

class FtSnowballState {
public:
	FtMemPool *pool;
	struct sb_stemmer *engine;
	CHARSET_INFO *engine_charset;
	enum FtNormalization normalization;
	bool unicode_v32;
	FtSnowballState(CHARSET_INFO *cs);
	~FtSnowballState();
};

#endif
