/* SPDX-License-Identifier: GPL-2.0-or-later */
/**
 * @file vfs_shell.c
 * @brief Shell commands on the VFS: mount, umount, ls, cat, df and write.
 *
 * Each command is a thin wrapper around the public VFS calls; failures are printed and returned
 * as the negative errno.
 */

#include <stdio.h>
#include <string.h>

#include <shell.h>
#include <size.h>
#include <vfs.h>

/**
 * @brief Print a command failure and return its error.
 *
 * @param[in] what     Command name.
 * @param[in] argument Argument to name, or NULL.
 * @param[in] error    Negative errno to report.
 * @return @p error, so the caller can return it directly.
 */
static int fail(const char *what, const char *argument, int error)
{
	printf("%s %s: %d\n", what, argument != NULL ? argument : "", error);
	return error;
}

/**
 * @brief "mount": list the mounts, or mount a device.
 *
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return 0 on success, or the negative errno from vfs_mount().
 *
 * With fewer than three arguments every mount table entry is printed. Otherwise the device,
 * target and optional type are passed to vfs_mount().
 */
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

/**
 * @brief "umount": unmount a target.
 *
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return 0 on success, -EINVAL without a target, or the negative errno from vfs_umount().
 */
static int command_umount(int argc, char **argv)
{
	int result = argc < 2 ? -EINVAL : vfs_umount(argv[1]);

	return result != 0 ? fail("umount", argc > 1 ? argv[1] : NULL, result) : 0;
}
SHELL_CMD(umount, "umount <target>", command_umount);

/**
 * @brief "ls": list a directory.
 *
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return 0 on success, or the negative errno that stopped the listing.
 *
 * The path defaults to "/". Each entry is printed with its size and a directory marker;
 * the walk stops at the first readdir error, which is treated as the end.
 */
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

/**
 * @brief "cat": print a file to the console.
 *
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return 0 on success, -EINVAL without a path, or the negative errno from vfs_open().
 *
 * The file is read in 256-byte chunks and each chunk is printed as a C string, so binary data
 * is not suitable for this command.
 */
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

/**
 * @brief "df": print the capacity and free space of a file system.
 *
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return 0 on success, -EINVAL without a path, or the negative errno from vfs_statfs().
 *
 * Both sizes are formatted with format_capacity().
 */
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

/**
 * @brief "write": replace a file with a text argument.
 *
 * @param[in] argc Argument count.
 * @param[in] argv Argument vector.
 * @return 0 on success, -EINVAL with too few arguments, or the negative errno that failed.
 *
 * The file is opened write-only with create and truncate, the text is written in one call and
 * the file is closed; the close error is only reported when the write itself succeeded.
 */
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

/**
 * @brief Remove one file from the VFS.
 *
 * @param[in] argc Argument count, at least two.
 * @param[in] argv Argument vector; @c argv[1] is the path.
 * @return 0 on success, or the VFS error.
 */
static int command_remove(int argc, char **argv)
{
	int result;

	if (argc < 2) {
		return -EINVAL;
	}
	result = vfs_unlink(argv[1]);
	return result < 0 ? fail("rm", argv[1], result) : 0;
}
SHELL_CMD(rm, "rm <path>", command_remove);

/**
 * @brief Remove one empty directory from the VFS.
 *
 * @param[in] argc Argument count, at least two.
 * @param[in] argv Argument vector; @c argv[1] is the path.
 * @return 0 on success, or the VFS error.
 */
static int command_rmdir(int argc, char **argv)
{
	int result;

	if (argc < 2) {
		return -EINVAL;
	}
	result = vfs_rmdir(argv[1]);
	return result < 0 ? fail("rmdir", argv[1], result) : 0;
}
SHELL_CMD(rmdir, "rmdir <path>", command_rmdir);
