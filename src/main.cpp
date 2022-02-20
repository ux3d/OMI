#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <mpg123.h>

#include <AL/al.h>
#include <AL/alext.h>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

bool loadFile(std::string& output, const std::string& filename)
{
	std::ifstream file(filename, std::ios::ate | std::ios::binary);
	if (!file.is_open())
	{
		return false;
	}

	size_t fileSize = static_cast<size_t>(file.tellg());
	file.seekg(0);

	output.resize(fileSize);

	file.read(output.data(), fileSize);
	file.close();

	return true;
}

ALuint createSound(const std::string& filename)
{
	// MPG123 for decoding

	if (mpg123_init() != MPG123_OK)
	{
		return 0;
	}

	mpg123_handle* handle = mpg123_new(0, 0);
	if (!handle)
	{
		return 0;
	}

	size_t buffer_size = mpg123_outblock(handle);
	std::vector<uint8_t> buffer_read(buffer_size);

	if (mpg123_open(handle, filename.c_str()) != MPG123_OK)
	{
		mpg123_delete(handle);

		return 0;
	}

	long rate;
	int channels, encoding;
	if (mpg123_getformat(handle, &rate, &channels, &encoding) != MPG123_OK)
	{
		mpg123_delete(handle);

		return 0;
	}

	size_t done;
	std::vector<uint8_t> decoded;
	while (mpg123_read(handle, buffer_read.data(), buffer_size, &done) == MPG123_OK)
	{
		decoded.insert(decoded.end(), buffer_read.begin(), buffer_read.end());
	}

	mpg123_delete(handle);
	mpg123_exit();

	// OpenAL buffer creation

	ALenum format = AL_NONE;

	if(channels == 1)
	{
		format = AL_FORMAT_MONO16;
	}
	else if(channels == 2)
	{
		format = AL_FORMAT_STEREO16;
	}
	else
	{
		printf("Error: Unsupported channel count %d\n", channels);

		return 0;
	}

	ALuint buffer;
	alGenBuffers(1, &buffer);

	alBufferData(buffer, format, decoded.data(), decoded.size(), rate);

	ALenum error = alGetError();
	if(error != AL_NO_ERROR)
	{
		printf("Error: OpenAL %s\n", alGetString(error));

		if(buffer && alIsBuffer(buffer))
		{
			alDeleteBuffers(1, &buffer);
		}

		return 0;
	}

    return buffer;
}

int main(int argc, char *argv[])
{
	// OpenAL init

	ALCdevice* device = 0;
	ALCcontext* context = 0;

	device = alcOpenDevice(NULL);
	if(!device)
	{
		printf("Error: Could not open device\n");

		return 1;
	}

	context = alcCreateContext(device, 0);
	if(!context)
	{
		printf("Error: Could not create context\n");

		return 1;
	}

	if (alcMakeContextCurrent(context) == ALC_FALSE)
	{
        printf("Error: Could not set context\n");

        alcDestroyContext(context);
        alcCloseDevice(device);

        return 1;
	}

	//

	std::string parentPath = "";
	std::string exampleFilename = "example01.gltf";

	std::string loadFilename = parentPath + exampleFilename;

	std::string gltfContent;
	if (!loadFile(gltfContent, loadFilename))
	{
		printf("Error: Could not load glTF file '%s'\n", loadFilename.c_str());

		return -1;
	}

	json glTF = json::parse(gltfContent);
	if (!glTF.contains("extensions"))
	{
		printf("Error: glTF does not contain any extensions\n");

		return -1;
	}

	json& extensions = glTF["extensions"];
	if (!extensions.contains("OMI_audio_emitter"))
	{
		printf("Error: glTF does not contain the OMI_audio_emitter extension\n");

		return -1;
	}

	// For now on, we expect the glTF does contain valid and the required data.

	std::vector<ALuint> audioSources;

	json& OMI_audio_emitter = extensions["OMI_audio_emitter"];
	for (auto& audioSource : OMI_audio_emitter["audioSources"])
	{
		if (!audioSource.contains("uri"))
		{
			printf("Error: Only supporting audioSource with uri\n");

			return -1;
		}

		if (audioSource["uri"].get<std::string>().find("data:application/") == 0)
		{
			printf("Error: Only supporting audioSource with uri containing a filename\n");

			return -1;
		}

		std::string uri = audioSource["uri"].get<std::string>();
		loadFilename = parentPath + uri;

		ALuint soundBuffer = createSound(loadFilename.c_str());
		if (soundBuffer == 0)
		{
			printf("Error: Could not create sound for file '%s'\n", loadFilename.c_str());

			return -1;
		}
		audioSources.push_back(soundBuffer);

		printf("Info: Loaded audio source with uri '%s'\n", uri.c_str());
	}

	// OpenAL shutdown

	for (ALuint buffer : audioSources)
	{
		alDeleteBuffers(1, &buffer);
	}

	alcMakeContextCurrent(0);

	alcDestroyContext(context);
	alcCloseDevice(device);

	//

	printf("Info: Done\n");

	return 0;
}
