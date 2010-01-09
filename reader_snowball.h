#include "reader.h"
#include "buffer.h"
#include "libstemmer_c/include/libstemmer.h"

class FtSnowballReader : public FtCharReader {
	FtCharReader *feeder;
	bool feeder_feed;
	struct sb_stemmer *engine;
	CHARSET_INFO *engine_charset;
	
	FtMemBuffer *input;
	FtMemReader *output;
	bool wc_sp;
	my_wc_t wc_in;
	int meta_in;
public:
	FtSnowballReader(FtCharReader *feeder, struct sb_stemmer *engine, CHARSET_INFO *engine_charset);
	~FtSnowballReader();
	bool readOne(my_wc_t *wc, int *meta);
	void reset();
};
