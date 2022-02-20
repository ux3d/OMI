#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <sndfile.h>

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

	std::vector<std::string> audoSources;

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

		std::string mpeg;
		if (!loadFile(mpeg, loadFilename))
		{
			printf("Error: Could not load mpeg file '%s'\n", loadFilename.c_str());

			return -1;
		}

		printf("Info: Loaded audio source with uri '%s'\n", uri.c_str());
	}

	// OpenAL shutdown

	alcMakeContextCurrent(0);

	alcDestroyContext(context);
	alcCloseDevice(device);

	//

	printf("Info: Done\n");

	return 0;
}
