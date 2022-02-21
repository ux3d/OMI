#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <AL/al.h>
#include <AL/alext.h>

#include <nlohmann/json.hpp>

#include "io.h"

#include "decode.h"

using json = nlohmann::json;

//
// OMI structures
//

struct AudioSource {
	ALuint buffer;
};

struct AudioEmitter {
	std::string type = "global";
	uint32_t audioSourceIndex;
	bool playing = false;
	ALboolean loop = AL_FALSE;
	ALfloat gain = 1.0f;
	std::string distanceModel = "inverse";
	ALfloat maxDistance = 10000.0f;
	ALfloat refDistance = 1.0f;
	ALfloat rolloffFactor = 1.0f;
};

struct AudioEmitterInstance {
	uint32_t audioEmitterIndex;
	ALuint source;
	//
	glm::vec4 position;
};

//
// Globals
//

static ALCdevice* g_device = 0;
static ALCcontext* g_context = 0;
static std::vector<AudioSource> g_audioSources;
static std::vector<AudioEmitter> g_audioEmitters;
static std::vector<AudioEmitterInstance> g_audioEmitterInstances;

static glm::vec4 g_listenerPosition(0.0f, 0.0f, 0.0f, 1.0f);
static glm::vec3 g_listenerVelocity(0.0f, 0.0f, 0.0f);
static glm::vec3 g_listenerForward(0.0f, 0.0f, -1.0f);
static glm::vec3 g_listenerUp(0.0f, 1.0f, 0.0f);

//
// Functions
//

ALuint createAudioBuffer(const AudioData& audioData)
{
	ALuint buffer;
	alGenBuffers(1, &buffer);

	alBufferData(buffer, AL_FORMAT_MONO16, audioData.decoded.data(), audioData.decoded.size(), audioData.frequency);

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

				audioEmitterInstance.position = currentPosition;

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
	// Audio initializing
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

	// OMI_audio_emitter requires a distance model per source, so we do it manually
	alDistanceModel(AL_NONE);

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

		//

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

		if (currentAudioEmitter.contains("type"))
		{
			audioEmitter.type = currentAudioEmitter["type"].get<std::string>();
		}
		if (currentAudioEmitter.contains("source"))
		{
			audioEmitter.audioSourceIndex = currentAudioEmitter["source"].get<uint32_t>();
		}
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
		if (currentAudioEmitter.contains("distanceModel"))
		{
			audioEmitter.distanceModel = currentAudioEmitter["distanceModel"].get<std::string>();
		}
		if (currentAudioEmitter.contains("maxDistance"))
		{
			audioEmitter.maxDistance = (ALfloat)currentAudioEmitter["maxDistance"].get<float>();
		}
		if (currentAudioEmitter.contains("refDistance"))
		{
			audioEmitter.refDistance = (ALfloat)currentAudioEmitter["refDistance"].get<float>();
		}
		if (currentAudioEmitter.contains("rolloffFactor"))
		{
			audioEmitter.rolloffFactor = (ALfloat)currentAudioEmitter["rolloffFactor"].get<float>();
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

		// Position etc. of listener
		alListenerfv(AL_POSITION, glm::value_ptr(g_listenerPosition));
		alListenerfv(AL_VELOCITY, glm::value_ptr(g_listenerVelocity));
		ALfloat listenerOrientation[6] = {g_listenerForward.x, g_listenerForward.y, g_listenerForward.z, g_listenerUp.x, g_listenerUp.y, g_listenerUp.z};
		alListenerfv(AL_ORIENTATION, listenerOrientation);

		for (auto& audioEmitterInstance : g_audioEmitterInstances)
		{
			auto& audioEmitter = g_audioEmitters[audioEmitterInstance.audioEmitterIndex];

			// Positional audio emitter
			if (audioEmitter.type == "positional")
			{
				float finalGain = audioEmitter.gain;

				ALfloat distance = glm::distance(audioEmitterInstance.position, g_listenerPosition);

				if (audioEmitter.distanceModel == "linear")
				{
					finalGain *= 1.0f - audioEmitter.rolloffFactor * (distance - audioEmitter.refDistance) / (audioEmitter.maxDistance - audioEmitter.refDistance);
				}
				else if (audioEmitter.distanceModel == "inverse")
				{
					finalGain *= audioEmitter.refDistance / (audioEmitter.refDistance + audioEmitter.rolloffFactor * (glm::max(distance, audioEmitter.refDistance) - audioEmitter.refDistance));
				}
				else if (audioEmitter.distanceModel == "exponential")
				{
					finalGain *= powf(glm::max(distance, audioEmitter.refDistance) / audioEmitter.refDistance, -audioEmitter.rolloffFactor);
				}

				alSourcef(audioEmitterInstance.source, AL_GAIN, finalGain);
				alSourcefv(audioEmitterInstance.source, AL_POSITION, glm::value_ptr(audioEmitterInstance.position));
			}

			//

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
	// Audio shutdown
	//

	shutdownAudio();

	return 0;
}
