/// @file vfs.h
/// @brief Headers for Virtual File System (VFS).
/// @copyright (c) 2014-2024 This file is distributed under the MIT License.
/// See LICENSE.md for details.

#pragma once

#include "fs/vfs_types.h"
#include "mem/alloc/slab.h"
#include "os_root_path.h"

/// Maximum number of opened file.
#define MAX_OPEN_FD 16

/// @brief Forward declaration of task_struct.
/// Used for task management in the VFS.
struct task_struct;

/// @brief Initialize the Virtual File System (VFS).
/// This function sets up necessary resources and structures for the VFS. It
/// must be called before any other VFS functions.
void vfs_init(void);

/// @brief Allocates a VFS file structure from the cache.
/// @param file Source file where the allocation is requested (for logging).
/// @param fun Function name where the allocation is requested (for logging).
/// @param line Line number where the allocation is requested (for logging).
/// @return Pointer to the allocated VFS file structure, or NULL if allocation fails.
vfs_file_t *pr_vfs_alloc_file(const char *file, const char *fun, int line);

/// @brief Frees a VFS file structure back to the cache.
/// @param file Source file where the deallocation is requested (for logging).
/// @param fun Function name where the deallocation is requested (for logging).
/// @param line Line number where the deallocation is requested (for logging).
/// @param vfs_file Pointer to the VFS file structure to free.
void pr_vfs_dealloc_file(const char *file, const char *fun, int line, vfs_file_t *vfs_file);

/// Wrapper that provides the filename, the function and line where the alloc is happening.
#define vfs_alloc_file(...) pr_vfs_alloc_file(__RELATIVE_PATH__, __func__, __LINE__)

/// Wrapper that provides the filename, the function and line where the free is happening.
#define vfs_dealloc_file(...) pr_vfs_dealloc_file(__RELATIVE_PATH__, __func__, __LINE__, __VA_ARGS__)

/// @brief Register a new filesystem.
/// @param fs A pointer to the information concerning the new filesystem.
/// @return The outcome of the operation, 0 if fails.
int vfs_register_filesystem(file_system_type_t *fs);

/// @brief Unregister a new filesystem.
/// @param fs A pointer to the information concerning the filesystem.
/// @return The outcome of the operation, 0 if fails.
int vfs_unregister_filesystem(file_system_type_t *fs);

/// @brief Register a superblock for the filesystem.
/// @param name The name of the superblock.
/// @param path The path associated with the superblock.
/// @param type A pointer to the filesystem type.
/// @param root A pointer to the root file of the filesystem.
/// @return 1 on success, 0 on failure.
int vfs_register_superblock(const char *name, const char *path, file_system_type_t *type, vfs_file_t *root);

/// @brief Unregister a superblock.
/// @param sb A pointer to the superblock to unregister.
/// @return 1 on success, 0 on failure.
int vfs_unregister_superblock(super_block_t *sb);

/// @brief Searches for the mountpoint of the given path.
/// @param absolute_path Path for which we want to search the mountpoint.
/// @return Pointer to the super_block_t of the mountpoint, or NULL if not found.
super_block_t *vfs_get_superblock(const char *absolute_path);

/// @brief Dumps the list of all superblocks to the log.
/// @param log_level Logging level to use for the output.
void vfs_dump_superblocks(int log_level);

/// @brief Open a file given its absolute path.
/// @param absolute_path An absolute path to the file.
/// @param flags Used to set the file status flags and access modes.
/// @param mode Specifies the file mode bits to apply when creating a new file.
/// @return Pointer to the opened vfs_file_t structure, or NULL on error.
vfs_file_t *vfs_open_abspath(const char *absolute_path, int flags, mode_t mode);

/// @brief Given a pathname for a file, vfs_open() returns a file struct, used to access the file.
/// @param path A pathname for a file.
/// @param flags Used to set the file status flags and file access modes of the open file description.
/// @param mode Specifies the file mode bits be applied when a new file is created.
/// @return Returns a file struct, or NULL.
vfs_file_t *vfs_open(const char *path, int flags, mode_t mode);

/// @brief Decreases the number of references to a given file, if the
///         references number reaches 0, close the file.
/// @param file A pointer to the file structure.
/// @return The result of the call.
int vfs_close(vfs_file_t *file);

/// @brief        Read data from a file.
/// @param file   The file structure used to reference a file.
/// @param buf    The buffer.
/// @param offset The offset from which the function starts to read.
/// @param nbytes The number of bytes to read.
/// @return The number of read characters.
ssize_t vfs_read(vfs_file_t *file, void *buf, size_t offset, size_t nbytes);

/// @brief        Write data to a file.
/// @param file   The file structure used to reference a file.
/// @param buf    The buffer.
/// @param offset The offset from which the function starts to write.
/// @param nbytes The number of bytes to write.
/// @return The number of written characters.
ssize_t vfs_write(vfs_file_t *file, const void *buf, size_t offset, size_t nbytes);

/// @brief Repositions the file offset inside a file.
/// @param file   The file for which we reposition the offest.
/// @param offset The offest to use for the operation.
/// @param whence The type of operation.
/// @return  Upon successful completion, returns the resulting offset
/// location as measured in bytes from the beginning of the file. On
/// error, the value (off_t) -1 is returned and errno is set to
/// indicate the error.
off_t vfs_lseek(vfs_file_t *file, off_t offset, int whence);

/// Provide access to the directory entries.
/// @param file  The directory for which we accessing the entries.
/// @param dirp  The buffer where de data should be placed.
/// @param off   The offset from which we start reading the entries.
/// @param count The size of the buffer.
/// @return On success, the number of bytes read is returned.  On end of
///         directory, 0 is returned.  On error, -1 is returned, and errno is set
///         appropriately.
ssize_t vfs_getdents(vfs_file_t *file, dirent_t *dirp, off_t off, size_t count);

/// @brief Perform the I/O control operation specified by `request` on `file`.
/// @param file The file for which the operation is executed.
/// @param request The device-dependent request code.
/// @param data An untyped value or pointer, depending on the request.
/// @return A request-specific return value, or a negative error code on failure.
long vfs_ioctl(vfs_file_t *file, unsigned int request, unsigned long data);

/// @brief Provides control operations on an open file descriptor.
/// @param file The file for which the operation is executed.
/// @param request The `fcntl` command, defining the operation (e.g., `F_GETFL`, `F_SETFL`).
/// @param data Additional data required by certain `fcntl` commands (e.g., flags or pointer).
/// @return Returns 0 on success; on error, returns a negative error code.
long vfs_fcntl(vfs_file_t *file, unsigned int request, unsigned long data);

/// @brief Delete a name and possibly the file it refers to.
/// @param path The path to the file.
/// @return On success, zero is returned. On error, -1 is returned, and
/// errno is set appropriately.
int vfs_unlink(const char *path);

/// @brief Creates a new directory at the given path.
/// @param path The path of the new directory.
/// @param mode The permission of the new directory.
/// @return Returns a negative value on failure.
int vfs_mkdir(const char *path, mode_t mode);

/// @brief Removes the given directory.
/// @param path The path to the directory to remove.
/// @return Returns a negative value on failure.
int vfs_rmdir(const char *path);

/// @brief Creates a new file or rewrite an existing one.
/// @param path path to the file.
/// @param mode mode for file creation.
/// @return file descriptor number, -1 otherwise and errno is set to indicate the error.
/// @details
/// It is equivalent to: open(path, O_WRONLY|O_CREAT|O_TRUNC, mode)
vfs_file_t *vfs_creat(const char *path, mode_t mode);

/// @brief Read the symbolic link, if present.
/// @param path the path to the symbolic link.
/// @param buffer the buffer where we will store the symbolic link path.
/// @param bufsize the size of the buffer.
/// @return The number of read characters on success, -1 otherwise and errno is set to indicate the error.
ssize_t vfs_readlink(const char *path, char *buffer, size_t bufsize);

/// @brief Creates a symbolic link.
/// @param linkname the name of the link.
/// @param path the entity it is linking to.
/// @return 0 on success, a negative number if fails and errno is set.
int vfs_symlink(const char *linkname, const char *path);

/// @brief Stat the file at the given path.
/// @param path Path to the file for which we are retrieving the statistics.
/// @param buf  Buffer where we are storing the statistics.
/// @return 0 on success, -errno on failure.
int vfs_stat(const char *path, stat_t *buf);

/// @brief Stat the given file.
/// @param file Pointer to the file for which we are retrieving the statistics.
/// @param buf  Buffer where we are storing the statistics.
/// @return 0 on success, -errno on failure.
int vfs_fstat(vfs_file_t *file, stat_t *buf);

/// @brief Mount the path as a filesystem of the given type.
/// @param type The type of filesystem
/// @param path The path to where it should be mounter.
/// @param args The arguments passed to the filesystem mount callback.
/// @return 0 on success, a negative number if fails and errno is set.
int vfs_mount(const char *type, const char *path, const char *args);

/// @brief Locks the access to the given file.
/// @param file The file to lock.
void vfs_lock(vfs_file_t *file);

/// @brief Extends the file descriptor list for the given task.
/// @param task The task for which we extend the file descriptor list.
/// @return 0 on fail, 1 on success.
int vfs_extend_task_fd_list(struct task_struct *task);

/// @brief Initialize the file descriptor list for the given task.
/// @param task The task for which we initialize the file descriptor list.
/// @return 0 on fail, 1 on success.
int vfs_init_task(struct task_struct *task);

/// @brief Duplicate the file descriptor list of old_task into new_task.
/// @param new_task The task where we clone the file descriptor list.
/// @param old_task The task from which we clone the file descriptor list.
/// @return 0 on fail, 1 on success.
int vfs_dup_task(struct task_struct *new_task, struct task_struct *old_task);

/// @brief Destroy the file descriptor list for the given task.
/// @param task The task for which we destroy the file descriptor list.
/// @return 0 on fail, 1 on success.
int vfs_destroy_task(struct task_struct *task);

/// @brief Find the smallest available fd.
/// @return -errno on fail, fd on success.
int get_unused_fd(void);

/// @brief Return new smallest available file desriptor.
/// @param fd the descriptor of the file we want to duplicate.
/// @return -errno on fail, fd on success.
int sys_dup(int fd);

/// @brief Check if the requested open flags against the file mask.
/// @param flags The requested open flags.
/// @param mask The permissions of the file.
/// @param uid The owner of the task opening the file.
/// @param gid The group of the task opening the file.
/// @return 0 on fail, 1 on success.
int vfs_valid_open_permissions(int flags, mode_t mask, uid_t uid, gid_t gid);

/// @brief Check if the file is exectuable
/// @param task The task to execute the file.
/// @param file The file to execute.
/// @return 0 on fail, 1 on success.
int vfs_valid_exec_permission(struct task_struct *task, vfs_file_t *file);
