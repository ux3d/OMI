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
// KHR audio structures
//

static const float M_PI = 3.1415926535897932384626433832795f;
static const float M_2PI = 2.0f * M_PI;

struct AudioSource {
	ALuint buffer;
};

struct Positional {
	std::string distanceModel = "inverse";
	ALfloat maxDistance = 10000.0f;
	ALfloat refDistance = 1.0f;
	ALfloat rolloffFactor = 1.0f;
	//
	ALfloat coneInnerAngle = M_2PI;
	ALfloat coneOuterAngle = M_2PI;
	ALfloat coneOuterGain = 0.0f;
};

struct AudioEmitter {
	std::string type = "global";
	uint32_t audioSourceIndex;
	bool playing = false;
	ALboolean loop = AL_FALSE;
	ALfloat gain = 1.0f;

	Positional positional;
};

struct Node {
	glm::vec4 position = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
	glm::vec3 orientation = glm::vec3(0.0f, 0.0f, 1.0f);
};

struct AudioEmitterInstance {
	uint32_t audioEmitterIndex;
	ALuint source;
	//
	uint32_t nodeIndex;
};

//
// Globals
//

static ALCdevice* g_device = 0;
static ALCcontext* g_context = 0;
static std::vector<AudioSource> g_audioSources;
static std::vector<AudioEmitter> g_audioEmitters;
static std::vector<AudioEmitterInstance> g_audioEmitterInstances;
static std::vector<Node> g_nodes;

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

	ALenum error = alGetError();
	if (error != AL_NO_ERROR)
	{
		printf("Error: OpenAL %s\n", alGetString(error));

		if(source && alIsSource(source))
		{
			alDeleteSources(1, &source);
		}

		return 0;
	}

	return source;
}

bool createAudioEmitterInstances(json& nodes, json& glTF)
{
	for (auto& currentNodeIndex : nodes)
	{
		json& currentNode = glTF["nodes"][currentNodeIndex.get<uint32_t>()];

		if (currentNode.contains("extensions"))
		{
			if (currentNode["extensions"].contains("KHR_audio"))
			{
				json& audioEmitter = currentNode["extensions"]["KHR_audio"]["emitter"];

				AudioEmitterInstance audioEmitterInstance;

				audioEmitterInstance.audioEmitterIndex = audioEmitter.get<uint32_t>();

				audioEmitterInstance.source = createAudioSource(g_audioEmitters[audioEmitterInstance.audioEmitterIndex]);
				if (!audioEmitterInstance.source)
				{
					return false;
				}

				audioEmitterInstance.nodeIndex = currentNodeIndex.get<uint32_t>();

				g_audioEmitterInstances.push_back(audioEmitterInstance);

				printf("Info: Created instance for audio emitter %u required for node\n", audioEmitterInstance.audioEmitterIndex);
			}
		}

		//

		if (currentNode.contains("children"))
		{
			if (!createAudioEmitterInstances(currentNode["children"], glTF))
			{
				return false;
			}
		}
	}

	return true;
}

bool setupAudio()
{
	g_device = alcOpenDevice(NULL);
	if(!g_device)
	{
		return false;
	}

	g_context = alcCreateContext(g_device, 0);
	if(!g_context)
	{
		return false;
	}

	if (alcMakeContextCurrent(g_context) == ALC_FALSE)
	{
        return false;
	}

	// KHR_audio requires a distance model per source, so we do it manually
	alDistanceModel(AL_NONE);

	ALenum error = alGetError();
	if (error != AL_NO_ERROR)
	{
		printf("Error: OpenAL %s\n", alGetString(error));

		return false;
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

void updateNodes(json& nodes, json& glTF, glm::mat4& parent)
{
	for (auto& currentNodeIndex : nodes)
	{
		json& currentNode = glTF["nodes"][currentNodeIndex.get<uint32_t>()];

		glm::mat4 local(1.0f);

		if (currentNode.contains("matrix"))
		{
			local = glm::mat4(
				currentNode["translation"]["matrix"][0].get<float>() , currentNode["translation"]["matrix"][1].get<float>() , currentNode["translation"]["matrix"][2].get<float>() , currentNode["translation"]["matrix"][3].get<float>() ,
				currentNode["translation"]["matrix"][4].get<float>() , currentNode["translation"]["matrix"][5].get<float>() , currentNode["translation"]["matrix"][6].get<float>() , currentNode["translation"]["matrix"][7].get<float>() ,
				currentNode["translation"]["matrix"][8].get<float>() , currentNode["translation"]["matrix"][9].get<float>() , currentNode["translation"]["matrix"][10].get<float>(), currentNode["translation"]["matrix"][11].get<float>(),
				currentNode["translation"]["matrix"][12].get<float>(), currentNode["translation"]["matrix"][13].get<float>(), currentNode["translation"]["matrix"][14].get<float>(), currentNode["translation"]["matrix"][15].get<float>()
			);
		}
		else
		{
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

				matrixRotation = glm::toMat4(rotation);
			}

			glm::mat4 matrixScale(1.0f);
			if (currentNode.contains("scale"))
			{
				glm::vec3 scale;
				scale.x = currentNode["scale"][0].get<float>();
				scale.y = currentNode["scale"][1].get<float>();
				scale.t = currentNode["scale"][2].get<float>();

				matrixScale = glm::scale(scale);
			}

			local = matrixTranslation * matrixRotation * matrixScale;
		}

		glm::mat4 world = parent * local;

		g_nodes[currentNodeIndex.get<uint32_t>()].position = world * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		g_nodes[currentNodeIndex.get<uint32_t>()].orientation = glm::mat3(world) * glm::vec3(0.0f, 0.0f, 1.0f);

		//

		if (currentNode.contains("children"))
		{
			updateNodes(currentNode["children"], glTF, world);
		}
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

	if (!setupAudio())
	{
        printf("Error: Could not setup audio\n");

        shutdownAudio();

		return -1;
	}

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
	if (!extensions.contains("KHR_audio"))
	{
		printf("Error: glTF does not contain the KHR_audio extension\n");

		shutdownAudio();

		return -1;
	}

	// For now on, we expect the glTF is valid and does contain the required data

	json& KHR_audio = extensions["KHR_audio"];

	for (auto& currentAudioSource : KHR_audio["sources"])
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

	for (auto& currentAudioEmitter : KHR_audio["emitters"])
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
		if (currentAudioEmitter.contains("positional") && audioEmitter.type == "positional")
		{
			if (currentAudioEmitter["positional"].contains("distanceModel"))
			{
				audioEmitter.positional.distanceModel = currentAudioEmitter["positional"]["distanceModel"].get<std::string>();
			}
			if (currentAudioEmitter["positional"].contains("maxDistance"))
			{
				audioEmitter.positional.maxDistance = (ALfloat)currentAudioEmitter["positional"]["maxDistance"].get<float>();
			}
			if (currentAudioEmitter["positional"].contains("refDistance"))
			{
				audioEmitter.positional.refDistance = (ALfloat)currentAudioEmitter["positional"]["refDistance"].get<float>();
			}
			if (currentAudioEmitter["positional"].contains("rolloffFactor"))
			{
				audioEmitter.positional.rolloffFactor = (ALfloat)currentAudioEmitter["positional"]["rolloffFactor"].get<float>();
			}
			//
			if (currentAudioEmitter["positional"].contains("coneInnerAngle"))
			{
				audioEmitter.positional.coneInnerAngle = (ALfloat)currentAudioEmitter["positional"]["coneInnerAngle"].get<float>();
			}
			if (currentAudioEmitter["positional"].contains("coneOuterAngle"))
			{
				audioEmitter.positional.coneOuterAngle = (ALfloat)currentAudioEmitter["positional"]["coneOuterAngle"].get<float>();
			}
			if (currentAudioEmitter["positional"].contains("coneOuterGain"))
			{
				audioEmitter.positional.coneOuterGain = (ALfloat)currentAudioEmitter["positional"]["coneOuterGain"].get<float>();
			}
		}

		g_audioEmitters.push_back(audioEmitter);

		printf("Info: Created audio emitter for audio source %u\n", audioEmitter.audioSourceIndex);
	}

	//

	if (glTF.contains("nodes"))
	{
		g_nodes.resize(glTF["nodes"].size());
	}

	//

	if (glTF.contains("scene"))
	{
		json& currentScene = glTF["scenes"][glTF["scene"].get<uint32_t>()];

		if (currentScene.contains("extensions"))
		{
			if (currentScene["extensions"].contains("KHR_audio"))
			{
				json& audioEmitters = currentScene["extensions"]["KHR_audio"]["emitters"];

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

			if (!createAudioEmitterInstances(currentScene["nodes"], glTF))
			{
				shutdownAudio();

				return -1;
			}
		}

		//

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

			// Note: Here one could update the listener position depending on user input

			// Position etc. of listener
			alListenerfv(AL_POSITION, glm::value_ptr(g_listenerPosition));
			alListenerfv(AL_VELOCITY, glm::value_ptr(g_listenerVelocity));
			ALfloat listenerOrientation[6] = {g_listenerForward.x, g_listenerForward.y, g_listenerForward.z, g_listenerUp.x, g_listenerUp.y, g_listenerUp.z};
			alListenerfv(AL_ORIENTATION, listenerOrientation);

			//

			// Note: Here one could update the node transforms with animations

			if (currentScene.contains("nodes"))
			{
				glm::mat4 world(1.0f);

				updateNodes(currentScene["nodes"], glTF, world);
			}

			//

			for (auto& audioEmitterInstance : g_audioEmitterInstances)
			{
				auto& audioEmitter = g_audioEmitters[audioEmitterInstance.audioEmitterIndex];

				// Positional audio emitter
				if (audioEmitter.type == "positional")
				{
					float finalGain = audioEmitter.gain;

					ALfloat distance = glm::distance(g_nodes[audioEmitterInstance.nodeIndex].position, g_listenerPosition);

					if (audioEmitter.positional.distanceModel == "linear")
					{
						finalGain *= 1.0f - audioEmitter.positional.rolloffFactor * (distance - audioEmitter.positional.refDistance) / (audioEmitter.positional.maxDistance - audioEmitter.positional.refDistance);
					}
					else if (audioEmitter.positional.distanceModel == "inverse")
					{
						finalGain *= audioEmitter.positional.refDistance / (audioEmitter.positional.refDistance + audioEmitter.positional.rolloffFactor * (glm::max(distance, audioEmitter.positional.refDistance) - audioEmitter.positional.refDistance));
					}
					else if (audioEmitter.positional.distanceModel == "exponential")
					{
						finalGain *= powf(glm::max(distance, audioEmitter.positional.refDistance) / audioEmitter.positional.refDistance, -audioEmitter.positional.rolloffFactor);
					}

					//

					if (glm::length(g_nodes[audioEmitterInstance.nodeIndex].orientation) != 0.0f && ((audioEmitter.positional.coneInnerAngle != M_2PI) || (audioEmitter.positional.coneOuterAngle != M_2PI)))
					{
						// Take sound cone into account

						glm::vec3 sourceToListener = glm::normalize(glm::vec3(g_nodes[audioEmitterInstance.nodeIndex].position - g_listenerPosition));
						glm::vec3 normalizedSourceOrientation = glm::normalize(g_nodes[audioEmitterInstance.nodeIndex].orientation);

						float angle = acosf(glm::dot(sourceToListener, normalizedSourceOrientation));
						float absAngle = fabs(angle);

						float absInnerAngle = fabs(audioEmitter.positional.coneInnerAngle) * 0.5f;
						float absOuterAngle = fabs(audioEmitter.positional.coneOuterAngle) * 0.5f;

						if (absAngle <= absInnerAngle)
						{
							// No attenuation
						}
						else if (absAngle >= absOuterAngle)
						{
							// Max attenuation
							finalGain *= audioEmitter.positional.coneOuterGain;
						}
						else
						{
						    // Between inner and outer cones
						    // inner -> outer, x goes from 0 -> 1
						    float x = (absAngle - absInnerAngle) / (absOuterAngle - absInnerAngle);

						    finalGain *= (1.0f - x) + audioEmitter.positional.coneOuterGain * x;
						}
					}

					//

					alSourcef(audioEmitterInstance.source, AL_GAIN, finalGain);
					alSourcefv(audioEmitterInstance.source, AL_POSITION, glm::value_ptr(g_nodes[audioEmitterInstance.nodeIndex].position));
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
	}

	//
	// Audio shutdown
	//

	shutdownAudio();

	return 0;
}
