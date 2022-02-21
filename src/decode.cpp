#include "decode.h"

#include <mpg123.h>

bool decodeAudioData(AudioData& audioData, const std::string& filename)
{
	//
	// mpg123 for decoding
	//

	if (mpg123_init() != MPG123_OK)
	{
		return false;
	}

	mpg123_handle* handle = mpg123_new(0, 0);
	if (!handle)
	{
		return false;
	}

	size_t buffer_size = mpg123_outblock(handle);
	std::vector<uint8_t> buffer_read(buffer_size);

	// Force to mono and 16bit
	if (mpg123_open_fixed(handle, filename.c_str(), MPG123_MONO, MPG123_ENC_SIGNED_16) != MPG123_OK)
	{
		mpg123_delete(handle);

		return false;
	}

	long rate;
	int channels, encoding;
	if (mpg123_getformat(handle, &rate, &channels, &encoding) != MPG123_OK)
	{
		mpg123_delete(handle);

		return false;
	}

	size_t done;
	while (mpg123_read(handle, buffer_read.data(), buffer_size, &done) == MPG123_OK)
	{
		audioData.decoded.insert(audioData.decoded.end(), buffer_read.begin(), buffer_read.end());
	}
	audioData.frequency = (int32_t)rate;

	mpg123_delete(handle);
	mpg123_exit();

	return true;
}
