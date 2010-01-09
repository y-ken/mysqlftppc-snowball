#include "reader_snowball.h"

FtSnowballReader::FtSnowballReader(FtCharReader *feeder, struct sb_stemmer *engine, CHARSET_INFO *engine_charset){
	this->feeder = feeder;
	feeder_feed = true;
	this->engine = engine;
	this->engine_charset = engine_charset;
	input = new FtMemBuffer(engine_charset);
	output = NULL;
	wc_sp = false;
}

FtSnowballReader::~FtSnowballReader(){
	delete input;
	if(output){
		delete output;
	}
}

bool FtSnowballReader::readOne(my_wc_t *wc, int *meta){
	if(output){
		if(output->readOne(wc, meta)){
			return true;
		}
		delete output;
		output = NULL;
	}
	if(wc_sp){
		wc_sp = false;
		*wc = wc_in;
		*meta = meta_in;
		return true;
	}
	if(!feeder_feed){
		*wc = FT_EOS;
		*meta = FT_CHAR_CTRL;
		return false;
	}
	input->reset();
	while(feeder_feed = feeder->readOne(&wc_in, &meta_in)){
//		fprintf(stderr,"snowball_got %lu %d\n", wc_in, meta_in); fflush(stderr);
		if(meta_in == FT_CHAR_NORM){
			input->append(wc_in);
		}else{
			wc_sp = true;
			break;
		}
	}
	input->flush();
	
	size_t length;
	size_t capacity;
	char *buffer=input->getBuffer(&length, &capacity);
	if(length > 0){
		const sb_symbol *sym = sb_stemmer_stem(engine, (const sb_symbol*)(buffer), length);
		if(sym){
			int sym_len = sb_stemmer_length(engine);
			output = new FtMemReader((const char*)sym, sym_len, engine_charset);
		}
	}
	return readOne(wc, meta);
}

void FtSnowballReader::reset(){
	feeder->reset();
	if(output){
		delete output;
		output = NULL;
	}
	wc_sp = false;
}
