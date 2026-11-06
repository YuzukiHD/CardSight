/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shell commands on the VFS: mount, umount, ls, cat, df. */

#include <stdio.h>
#include <string.h>

#include <shell.h>
#include <size.h>
#include <vfs.h>

static int fail(const char *what, const char *argument, int error)
{
	printf("%s %s: %d\n", what, argument != NULL ? argument : "", error);
	return error;
}

static int command_mount(int argc, char **argv)
{
	int result;

	if (argc < 3) {
		for (uint32_t index = 0U;; index++) {
			struct vfs_mount_info info;

			if (vfs_mount_get(index, &info) != 0) {
				break;
			}
			printf("%-10s on %-10s %s, %u open\n", info.blk, info.target, info.fstype, info.open_count);
		}
		return 0;
	}
	result = vfs_mount(argv[1], argv[2], argc > 3 ? argv[3] : NULL);
	return result != 0 ? fail("mount", argv[1], result) : 0;
}
SHELL_CMD(mount, "mount [blk target [fstype]]", command_mount);

static int command_umount(int argc, char **argv)
{
	int result = argc < 2 ? -EINVAL : vfs_umount(argv[1]);

	return result != 0 ? fail("umount", argc > 1 ? argv[1] : NULL, result) : 0;
}
SHELL_CMD(umount, "umount <target>", command_umount);

static int command_ls(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/";
	struct vfs_dirent entry;
	struct vfs_dir *dir;
	int error = 0;

	dir = vfs_opendir(path, &error);
	if (dir == NULL) {
		return fail("ls", path, error);
	}
	while (vfs_readdir(dir, &entry) == 0) {
		printf("  %-32s %8llu%s\n", entry.name, (unsigned long long)entry.st.size,
			entry.st.dir ? "  <dir>" : "");
	}
	vfs_closedir(dir);
	return 0;
}
SHELL_CMD(ls, "ls [path]", command_ls);

static int command_cat(int argc, char **argv)
{
	char buffer[256];
	int descriptor;
	int got;

	if (argc < 2) {
		return -EINVAL;
	}
	descriptor = vfs_open(argv[1], VFS_O_READ);
	if (descriptor < 0) {
		return fail("cat", argv[1], descriptor);
	}
	while ((got = vfs_read(descriptor, buffer, sizeof(buffer) - 1U)) > 0) {
		buffer[got] = '\0';
		printf("%s", buffer);
	}
	vfs_close(descriptor);
	return 0;
}
SHELL_CMD(cat, "cat <path>", command_cat);

static int command_df(int argc, char **argv)
{
	uint64_t total = 0U;
	uint64_t free_bytes = 0U;
	int result = argc < 2 ? -EINVAL : vfs_statfs(argv[1], &total, &free_bytes);

	if (result != 0) {
		return fail("df", argc > 1 ? argv[1] : NULL, result);
	}
	{
		char total_text[16];
		char free_text[16];

		format_capacity(total, total_text, sizeof(total_text));
		format_capacity(free_bytes, free_text, sizeof(free_text));
		printf("%s total, %s free\n", total_text, free_text);
	}
	return 0;
}
SHELL_CMD(df, "df <path>", command_df);

static int command_write(int argc, char **argv)
{
	int descriptor;
	size_t length;
	int result;

	if (argc < 3) {
		return -EINVAL;
	}
	descriptor = vfs_open(argv[1], VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
	if (descriptor < 0) {
		return fail("write", argv[1], descriptor);
	}
	length = strlen(argv[2]);
	result = vfs_write(descriptor, argv[2], length);
	if (result >= 0) {
		result = vfs_close(descriptor);
	} else {
		(void)vfs_close(descriptor);
	}
	return result < 0 ? fail("write", argv[1], result) : 0;
}
SHELL_CMD(write, "write <path> <text>", command_write);
