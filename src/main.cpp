#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <mpg123.h>

#include <AL/al.h>
#include <AL/alext.h>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

//
// Globals
//

static ALCdevice* g_device = 0;
static ALCcontext* g_context = 0;
static std::vector<ALuint> g_audioBuffers;
static std::vector<ALuint> g_audioSources;

//
// Functions
//

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

ALuint createAudioBuffer(const std::string& filename)
{
	//
	// mpg123 for decoding
	//

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

	std::vector<uint8_t> decoded;

	size_t done;
	while (mpg123_read(handle, buffer_read.data(), buffer_size, &done) == MPG123_OK)
	{
		decoded.insert(decoded.end(), buffer_read.begin(), buffer_read.end());
	}

	mpg123_delete(handle);
	mpg123_exit();

	//
	// OpenAL buffer creation
	//

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

void shutdownAudio()
{
	for (ALuint source : g_audioSources)
	{
		alDeleteSources(1, &source);
	}

	for (ALuint buffer : g_audioBuffers)
	{
		alDeleteBuffers(1, &buffer);
	}

	if (g_context)
	{
		alcMakeContextCurrent(0);

		alcDestroyContext(g_context);
	}

	if (g_device)
	{
		alcCloseDevice(g_device);
	}
}

//
// Main entry
//

int main(int argc, char *argv[])
{
	printf("Info: Starting\n");

	//
	// OpenAL initializing
	//

	g_device = alcOpenDevice(NULL);
	if(!g_device)
	{
		printf("Error: Could not open device\n");

		return 1;
	}

	g_context = alcCreateContext(g_device, 0);
	if(!g_context)
	{
		printf("Error: Could not create context\n");

		shutdownAudio();

		return 1;
	}

	if (alcMakeContextCurrent(g_context) == ALC_FALSE)
	{
        printf("Error: Could not set context\n");

        shutdownAudio();

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

		shutdownAudio();

		return -1;
	}

	json glTF = json::parse(gltfContent);
	if (!glTF.contains("extensions"))
	{
		printf("Error: glTF does not contain any extensions\n");

		shutdownAudio();

		return -1;
	}

	json& extensions = glTF["extensions"];
	if (!extensions.contains("OMI_audio_emitter"))
	{
		printf("Error: glTF does not contain the OMI_audio_emitter extension\n");

		shutdownAudio();

		return -1;
	}

	// For now on, we expect the glTF does contain valid and the required data.

	json& OMI_audio_emitter = extensions["OMI_audio_emitter"];
	for (auto& audioSource : OMI_audio_emitter["audioSources"])
	{
		if (!audioSource.contains("uri"))
		{
			printf("Error: Only supporting audioSource with uri\n");

			shutdownAudio();

			return -1;
		}

		if (audioSource["uri"].get<std::string>().find("data:application/") == 0)
		{
			printf("Error: Only supporting audioSource with uri containing a filename\n");

			shutdownAudio();

			return -1;
		}

		std::string uri = audioSource["uri"].get<std::string>();
		loadFilename = parentPath + uri;

		ALuint audioBuffer = createAudioBuffer(loadFilename.c_str());
		if (audioBuffer == 0)
		{
			printf("Error: Could not create sound for file '%s'\n", loadFilename.c_str());

			shutdownAudio();

			return -1;
		}
		g_audioBuffers.push_back(audioBuffer);

		printf("Info: Loaded audio source with uri '%s'\n", uri.c_str());
	}

	//


	for (ALuint buffer : g_audioBuffers)
	{
		ALuint audioSource = 0;
	    alGenSources(1, &audioSource);
	    alSourcei(audioSource, AL_BUFFER, (ALint)buffer);

	    if (alGetError() != AL_NO_ERROR)
	    {
	    	shutdownAudio();

	    	return -1;
	    }

	    g_audioSources.push_back(audioSource);
	}

	//
	// Test
	//

	ALint state;
	for (ALuint source : g_audioSources)
	{
		alSourcePlay(source);
		do {
			std::this_thread::yield();
			alGetSourcei(source, AL_SOURCE_STATE, &state);
		} while(alGetError() == AL_NO_ERROR && state == AL_PLAYING);
	}

	//
	// OpenAL shutdown
	//

	shutdownAudio();

	//

	printf("Info: Done\n");

	return 0;
}
