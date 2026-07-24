#include "DirectoryService.h"

#include " ReplyCodes.h"

DirectoryService::DirectoryService(const PathResolver& resolver) : resolver(resolver) {}

DirectoryService::Result DirectoryService::printWorkingDir(const std::filesystem::path& currentDir) const {
	auto relative = fs::relative(currentDir, resolver.root());
	std::string display = "/" + relative.generic_string();
	if (display == "/.") display = "/";

	return { true, ReplyCode::PathnameCreated, "\"" + display + "\" is the current directory" };
}

DirectoryService::Result DirectoryService::changeDir(fs::path& currentDir, const std::string& target) const {
	fs::path resolved;
	if (!resolver.resolve(currentDir, target, resolved))
		return { false, ReplyCode::ActionNotTaken, "Requested action not taken: path outside server root" };
	if(!fs::exists(resolved) || !fs::is_directory(resolved))
		return { false, ReplyCode::ActionNotTaken, "Requested action not taken: no a directory" };

	currentDir = resolved;
	return { true, ReplyCode::ActionCompleted, "Directory changed to " + resolved.filename().string() };
}

DirectoryService::Result DirectoryService::changeToParent(fs::path& currentDir) const {
	fs::path resolved;
	if (!resolver.resolve(currentDir, "..", resolved))
		return { false, ReplyCode::ActionNotTaken, "Requested action not taken: already at server root" };

	currentDir = resolved;
	return { true, ReplyCode::ActionCompleted, "Directory changed to " + resolved.filename().string() };
}

DirectoryService::Result DirectoryService::makeDir(const fs::path& currentDir, const std::string& name) const {
	fs::path resolved;
	if(!resolver.resolve(currentDir, name, resolved))
		return { false, ReplyCode::ActionNotTaken, "Requested action not taken: path outside server root" };
	if(fs::exists(resolved))
		return { false, ReplyCode::ActionNotTaken, "Requested action not taken: already exists" };

	std::error_code ec;
	if(!fs::create_directories(resolved, ec) || ec)
		return { false, ReplyCode::ActionNotTaken, "Requested action not taken: could not create directory" };

	return { true, ReplyCode::PathnameCreated, "\"" + resolved.filename().string() + "\" created" };
}

DirectoryService::Result DirectoryService::removeDir(const fs::path& currentDir, const std::string& name) const {
	fs::path resolved;
	if (!resolver.resolve(currentDir, name, resolved))
		return { false, ReplyCode::ActionNotTaken, "Requested action not taken: path outside server root" };
	if (!fs::exists(resolved) || !fs::is_directory(resolved))
		return { false, ReplyCode::ActionNotTaken, "Requested action not taken: not a directory" };
	if(resolved == resolver.root())
		return { false, ReplyCode::ActionNotTaken, "Requested action not taken: cannot remove server root" };

	std::error_code ec;
	if (!fs::remove(resolved, ec) || ec)
		return { false, ReplyCode::ActionNotTaken, "Requested action not taken: directory not empty or in use" };

	return { true, ReplyCode::ActionCompleted, "Directory removed" };
}

bool DirectoryService::listDir(const fs::path& currentDir, std::vector<DirEntryInfo>& outEntries) const {
	outEntries.clear();
	std::error_code ec;

	for (const auto& entry : fs::directory_iterator(currentDir, ec)) {
		DirEntryInfo info;
		info.formatPermissions = getFormatPermissions(entry.path());
		info.name = entry.path().filename().string();
		info.isDirectory = entry.is_directory();
		info.sizeBytes = info.isDirectory ? 0 : std::filesystem::file_size(entry.path(), ec);
		outEntries.push_back(info);
	}
	return !ec;
}

bool DirectoryService::getMetadata(const fs::path& currentDir, const std::string& target, PathMetadata& out) const {
	fs::path resolved;
	if (!resolver.resolve(currentDir, target, resolved)) return false;

	std::error_code ec;
	out.exists = fs::exists(resolved, ec);
	if (!out.exists || ec) return false;

	out.isDirectory = fs::is_directory(resolved, ec);
	out.sizeBytes = out.isDirectory ? 0 : fs::file_size(resolved, ec);
	out.lastModified = fs::last_write_time(resolved, ec);

	return !ec;
}

std::string DirectoryService::getFormatPermissions(const fs::path& path) const {
	std::error_code ec;
	fs::file_status status = fs::status(path, ec);
	if (ec) return "d---------";

	char type_char = '-';
	switch (status.type()) {
	case fs::file_type::directory:		type_char = 'd'; break;
		case fs::file_type::regular:    type_char = '-'; break;
		case fs::file_type::symlink:    type_char = 'l'; break;
		case fs::file_type::block:      type_char = 'b'; break;
		case fs::file_type::character:  type_char = 'c'; break;
		case fs::file_type::fifo:       type_char = 'p'; break;
		case fs::file_type::socket:     type_char = 's'; break;
		default:                        type_char = '-'; break;
	}

	std::string perms_str = "---------";
	fs::perms p = status.permissions();
	if ((p & fs::perms::owner_read) != fs::perms::none)	 perms_str[0] = 'r';
	if ((p & fs::perms::owner_write) != fs::perms::none) perms_str[1] = 'w';
	if ((p & fs::perms::owner_exec) != fs::perms::none)  perms_str[2] = 'x';

	if ((p & fs::perms::group_read) != fs::perms::none)  perms_str[3] = 'r';
	if ((p & fs::perms::group_write) != fs::perms::none) perms_str[4] = 'w';
	if ((p & fs::perms::group_exec) != fs::perms::none)  perms_str[5] = 'x';

	if ((p & fs::perms::others_read) != fs::perms::none)  perms_str[6] = 'r';
	if ((p & fs::perms::others_write) != fs::perms::none) perms_str[7] = 'w';
	if ((p & fs::perms::others_exec) != fs::perms::none)  perms_str[8] = 'x';

	return type_char + perms_str;
}
