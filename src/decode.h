#ifndef DECODE_H_
#define DECODE_H_

#include <cstdint>
#include <string>
#include <vector>

struct AudioData {
	std::vector<uint8_t> decoded;
	int32_t frequency;
};

bool decodeAudioData(AudioData& audioData, const std::string& filename);

#endif /* DECODE_H_ */
