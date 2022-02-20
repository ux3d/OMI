#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/transform.hpp>

#include <mpg123.h>

#include <AL/al.h>
#include <AL/alext.h>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct DecomposedPath {
	std::string parentPath = "";
	std::string stem = "";
	std::string extension = "";
};

struct AudioData {
	std::vector<uint8_t> decoded;
	int32_t frequency;
	int32_t channels;
};

struct AudioSource {
	ALuint buffer;
};

struct AudioEmitter {
	uint32_t audioSourceIndex;
	bool playing = false;
	ALboolean loop = AL_FALSE;
	ALfloat gain = 1.0f;
};

struct AudioEmitterInstance {
	uint32_t audioEmitterIndex;
	ALuint source;
	//
	glm::mat4 world;
};

//
// Globals
//

static ALCdevice* g_device = 0;
static ALCcontext* g_context = 0;
static std::vector<AudioSource> g_audioSources;
static std::vector<AudioEmitter> g_audioEmitters;
static std::vector<AudioEmitterInstance> g_audioEmitterInstances;

//
// Functions
//

void decomposePath(DecomposedPath& decomposedPath, const std::string& path)
{
	std::filesystem::path filesystemPath(path);

	decomposedPath.parentPath = filesystemPath.parent_path().generic_string();
	decomposedPath.stem = filesystemPath.stem().generic_string();
	decomposedPath.extension = filesystemPath.extension().generic_string();

	if (decomposedPath.parentPath.size() > 0 && (decomposedPath.parentPath.back() != '/' || decomposedPath.parentPath.back() != '\\'))
	{
		decomposedPath.parentPath += "/";
	}
}

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

	if (mpg123_open(handle, filename.c_str()) != MPG123_OK)
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
	audioData.channels = (int32_t)channels;

	mpg123_delete(handle);
	mpg123_exit();

	return true;
}

ALuint createAudioBuffer(const AudioData& audioData)
{
	//
	// OpenAL buffer creation
	//

	ALenum format = AL_NONE;

	if(audioData.channels == 1)
	{
		format = AL_FORMAT_MONO16;
	}
	else if(audioData.channels == 2)
	{
		format = AL_FORMAT_STEREO16;
	}
	else
	{
		printf("Error: Unsupported channel count %d\n", audioData.channels);

		return 0;
	}

	ALuint buffer;
	alGenBuffers(1, &buffer);

	alBufferData(buffer, format, audioData.decoded.data(), audioData.decoded.size(), audioData.frequency);

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

ALuint createAudioSource(const AudioEmitter& audioEmitter)
{
	ALuint source;

	alGenSources(1, &source);

	alSourcei(source, AL_BUFFER, (ALint)g_audioSources[audioEmitter.audioSourceIndex].buffer);

	alSourcei(source, AL_LOOPING, audioEmitter.loop);
	alSourcef(source, AL_GAIN, audioEmitter.gain);

	if (alGetError() != AL_NO_ERROR)
	{
		return 0;
	}

	return source;
}

bool handleNodes(json& nodes, json& glTF, glm::mat4& parent)
{
	for (auto& currentNodeIndex : nodes)
	{
		json& currentNode = glTF["nodes"][currentNodeIndex.get<uint32_t>()];

		glm::mat4 matrixTranslation(1.0f);
		if (currentNode.contains("translation"))
		{
			glm::vec3 translation;
			translation.x = currentNode["translation"][0].get<float>();
			translation.y = currentNode["translation"][1].get<float>();
			translation.t = currentNode["translation"][2].get<float>();

			matrixTranslation = glm::translate(translation);
		}

		glm::mat4 matrixRotation(1.0f);
		if (currentNode.contains("rotation"))
		{
			glm::quat rotation;
			rotation.w = currentNode["rotation"][3].get<float>();
			rotation.x = currentNode["rotation"][0].get<float>();
			rotation.y = currentNode["rotation"][1].get<float>();
			rotation.z = currentNode["rotation"][2].get<float>();

			matrixTranslation = glm::toMat4(rotation);
		}

		glm::mat4 matrixScale(1.0f);
		if (currentNode.contains("scale"))
		{
			glm::vec3 scale;
			scale.x = currentNode["scale"][0].get<float>();
			scale.y = currentNode["scale"][1].get<float>();
			scale.t = currentNode["scale"][2].get<float>();

			matrixTranslation = glm::scale(scale);
		}

		glm::mat4 local = matrixTranslation * matrixRotation * matrixScale;
		glm::mat4 world = parent * local;
		glm::vec4 currentPosition = world * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);

		//

		if (currentNode.contains("extensions"))
		{
			if (currentNode["extensions"].contains("OMI_audio_emitter"))
			{
				json& audioEmitter = currentNode["extensions"]["OMI_audio_emitter"]["audioEmitter"];

				AudioEmitterInstance audioEmitterInstance;
				audioEmitterInstance.audioEmitterIndex = audioEmitter.get<uint32_t>();

				audioEmitterInstance.source = createAudioSource(g_audioEmitters[audioEmitterInstance.audioEmitterIndex]);
				if (!audioEmitterInstance.source)
				{
					return false;
				}

				ALfloat position[3] = {currentPosition.x, currentPosition.y, currentPosition.z};
				alSourcefv(audioEmitterInstance.source, AL_POSITION, position);

				g_audioEmitterInstances.push_back(audioEmitterInstance);

				printf("Info: Created instance for audio emitter %u required for node\n", audioEmitterInstance.audioEmitterIndex);
			}
		}

		//

		if (currentNode.contains("children"))
		{
			if (!handleNodes(currentNode["children"], glTF, world))
			{
				return false;
			}
		}
	}

	return true;
}

void shutdownAudio()
{
	for (auto& audioEmitterInstance : g_audioEmitterInstances)
	{
		alDeleteSources(1, &audioEmitterInstance.source);
	}

	//

	for (auto& audioSource : g_audioSources)
	{
		alDeleteBuffers(1, &audioSource.buffer);
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
	std::string filename = "example01.gltf";
	if (argc > 1)
	{
		filename = argv[1];
	}

	DecomposedPath decomposedFilename;
	decomposePath(decomposedFilename, filename);

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

	ALfloat listenerPosition[3] = {0.0f, 0.0f, 0.0f};
	ALfloat listenerVelocity[3] = {0.0f, 0.0f, 0.0f};
	ALfloat listenerOrientation[6] = {0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f};
	alListenerfv(AL_POSITION, listenerPosition);
	alListenerfv(AL_VELOCITY, listenerVelocity);
	alListenerfv(AL_ORIENTATION, listenerOrientation);

	//
	// glTF loading and interpreting
	//

	std::string loadFilename = decomposedFilename.parentPath + decomposedFilename.stem + decomposedFilename.extension;

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

	// For now on, we expect the glTF is valid and does contain the required data

	json& OMI_audio_emitter = extensions["OMI_audio_emitter"];

	for (auto& currentAudioSource : OMI_audio_emitter["audioSources"])
	{
		if (!currentAudioSource.contains("uri"))
		{
			printf("Error: Only supporting audioBuffer with uri\n");

			shutdownAudio();

			return -1;
		}

		if (currentAudioSource["uri"].get<std::string>().find("data:application/") == 0)
		{
			printf("Error: Only supporting audioBuffer with uri containing a filename\n");

			shutdownAudio();

			return -1;
		}

		std::string uri = currentAudioSource["uri"].get<std::string>();

		loadFilename = decomposedFilename.parentPath + uri;

		AudioData audioData;
		if (!decodeAudioData(audioData, loadFilename))
		{
			printf("Error: Could not decode audio data for uri '%s'\n", uri.c_str());

			shutdownAudio();

			return -1;
		}

		AudioSource audioSource;

		audioSource.buffer = createAudioBuffer(audioData);
		if (audioSource.buffer == 0)
		{
			printf("Error: Could not create audio buffer for uri '%s'\n", uri.c_str());

			shutdownAudio();

			return -1;
		}

		g_audioSources.push_back(audioSource);

		printf("Info: Created audio source for uri '%s'\n", uri.c_str());
	}

	for (auto& currentAudioEmitter : OMI_audio_emitter["audioEmitters"])
	{
		AudioEmitter audioEmitter;

		audioEmitter.audioSourceIndex = currentAudioEmitter["source"].get<uint32_t>();

		if (currentAudioEmitter.contains("playing"))
		{
			audioEmitter.playing = currentAudioEmitter["playing"].get<bool>();
		}
		if (currentAudioEmitter.contains("loop"))
		{
			audioEmitter.loop = (ALboolean)currentAudioEmitter["loop"].get<bool>();
		}
		if (currentAudioEmitter.contains("gain"))
		{
			audioEmitter.gain = (ALfloat)currentAudioEmitter["gain"].get<float>();
		}

		g_audioEmitters.push_back(audioEmitter);

		printf("Info: Created audio emitter for audio source %u\n", audioEmitter.audioSourceIndex);
	}

	//

	if (glTF.contains("scene"))
	{
		json& currentScene = glTF["scenes"][glTF["scene"].get<uint32_t>()];

		if (currentScene.contains("extensions"))
		{
			if (currentScene["extensions"].contains("OMI_audio_emitter"))
			{
				json& audioEmitters = currentScene["extensions"]["OMI_audio_emitter"]["audioEmitters"];

				for (auto& currentAudioEmitter : audioEmitters)
				{
					AudioEmitterInstance audioEmitterInstance;
					audioEmitterInstance.audioEmitterIndex = currentAudioEmitter.get<uint32_t>();

					audioEmitterInstance.source = createAudioSource(g_audioEmitters[audioEmitterInstance.audioEmitterIndex]);
					if (!audioEmitterInstance.source)
					{
						shutdownAudio();

						return -1;
					}

					g_audioEmitterInstances.push_back(audioEmitterInstance);

					printf("Info: Created instance for audio emitter %u required for scene\n", audioEmitterInstance.audioEmitterIndex);
				}
			}
		}

		//

		if (currentScene.contains("nodes"))
		{
			glm::mat4 world(1.0f);

			if (!handleNodes(currentScene["nodes"], glTF, world))
			{
				shutdownAudio();

				return -1;
			}
		}
	}

	//
	// Start playing, if emitter is set
	//

	for (auto& audioEmitterInstance : g_audioEmitterInstances)
	{
		AudioEmitter& audioEmitter = g_audioEmitters[audioEmitterInstance.audioEmitterIndex];

		if (audioEmitter.playing)
		{
			alSourcePlay(audioEmitterInstance.source);
		}
	}

	//
	// Play, until all emitter instances are not playing anymore. Can be infinite, if one is looping
	//

	bool loop;
	do {
		std::this_thread::yield();
		loop = false;

		for (auto& audioEmitterInstance : g_audioEmitterInstances)
		{
			ALint state;
			alGetSourcei(audioEmitterInstance.source, AL_SOURCE_STATE, &state);

			if (state == AL_PLAYING)
			{
				loop = true;
			}

			if (alGetError() != AL_NO_ERROR)
			{
				loop = false;
			}
		}
	} while(loop);

	//
	// OpenAL shutdown
	//

	shutdownAudio();

	return 0;
}
