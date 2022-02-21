#ifndef IO_H_
#define IO_H_

#include <string>

struct DecomposedPath {
	std::string parentPath = "";
	std::string stem = "";
	std::string extension = "";
};

void decomposePath(DecomposedPath& decomposedPath, const std::string& path);

bool loadFile(std::string& output, const std::string& filename);

#endif /* IO_H_ */
