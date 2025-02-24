#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <getopt.h>
#else
#include <argp.h>
#include <parted/parted.h>
#endif

#include "version.h"
#include "libutils.h"

// Общие константы
#define DEFAULT_FIRST_SEC 2048
#define WIN_SECTOR_SIZE 512

// Глобальные переменные
const char *argp_program_version = "F3 Fix " F3_STR_VERSION;

// Структура аргументов
struct args {
    bool list_disk_types;
    bool list_fs_types;
    bool boot;
    const char *dev_filename;
    PedDiskType *disk_type; 
    PedFileSystemType *fs_type;
    long long first_sec;
    long long last_sec;
};

#ifdef _WIN32
// Windows-специфичные функции
static int fix_disk_win(const char *dev_path, long long start, long long end) {
    HANDLE hDevice = CreateFileA(dev_path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    
    if (hDevice == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Error opening device: %lu\n", GetLastError());
        return 0;
    }

    // Создание новой MBR
    CREATE_DISK createDisk = {0};
    createDisk.PartitionStyle = PARTITION_STYLE_MBR;
    DWORD bytesReturned;
    
    if (!DeviceIoControl(hDevice, IOCTL_DISK_CREATE_DISK,
            &createDisk, sizeof(createDisk), NULL, 0, &bytesReturned, NULL)) {
        fprintf(stderr, "Create disk failed: %lu\n", GetLastError());
        CloseHandle(hDevice);
        return 0;
    }

    // Настройка разделов
    DRIVE_LAYOUT_INFORMATION_EX layout = {0};
    layout.PartitionStyle = PARTITION_STYLE_MBR;
    layout.Mbr.Signature = 0x12345678;
    
    // Расчет параметров раздела
    PARTITION_INFORMATION_EX part = {0};
    part.PartitionStyle = PARTITION_STYLE_MBR;
    part.Mbr.PartitionType = 0x0C; // FAT32
    part.StartingOffset.QuadPart = start * WIN_SECTOR_SIZE;
    part.PartitionLength.QuadPart = (end - start + 1) * WIN_SECTOR_SIZE;
    part.Mbr.BootIndicator = args.boot;
    
    memcpy(&layout.Mbr.PartitionEntry[0], &part, sizeof(PARTITION_INFORMATION_EX));
    layout.PartitionCount = 1;

    if (!DeviceIoControl(hDevice, IOCTL_DISK_SET_DRIVE_LAYOUT_EX,
            &layout, sizeof(layout), NULL, 0, &bytesReturned, NULL)) {
        fprintf(stderr, "Set layout failed: %lu\n", GetLastError());
        CloseHandle(hDevice);
        return 0;
    }

    CloseHandle(hDevice);
    return 1;
}

// Парсинг аргументов для Windows
static void parse_args(int argc, char **argv, struct args *args) {
    int opt;
    static struct option long_options[] = {
        {"disk-type", required_argument, 0, 'd'},
        {"fs-type", required_argument, 0, 'f'},
        {"boot", no_argument, 0, 'b'},
        {"no-boot", no_argument, 0, 'n'},
        {"first-sec", required_argument, 0, 'a'},
        {"last-sec", required_argument, 0, 'l'},
        {"list-disk-types", no_argument, 0, 'k'},
        {"list-fs-types", no_argument, 0, 's'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "d:f:bn:a:l:ks", long_options, NULL)) != -1) {
        switch (opt) {
        case 'd': args->disk_type = optarg; break;
        case 'f': args->fs_type = optarg; break;
        case 'b': args->boot = true; break;
        case 'n': args->boot = false; break;
        case 'a': args->first_sec = atoll(optarg); break;
        case 'l': args->last_sec = atoll(optarg); break;
        case 'k': args->list_disk_types = true; break;
        case 's': args->list_fs_types = true; break;
        default: exit(EXIT_FAILURE);
        }
    }
    
    if (optind < argc) {
        args->dev_filename = argv[optind];
    }
}

#else // Linux реализация

static long long arg_to_long_long(const struct argp_state *state, const char *arg);
static void list_disk_types(void);
static void list_fs_types(void);
static PedSector map_sector_to_logical_sector(PedSector sector, int logical_sector_size);
static int fix_disk(PedDevice *dev, PedDiskType *type, PedFileSystemType *fs_type, int boot, PedSector start, PedSector end);

static char adoc[] = "<DISK_DEV>";
static char doc[] = "F3 Fix -- edit the partition table...";
static struct argp_option options[] = {
    {"disk-type",    'd',    "TYPE",    0, "Disk type", 2},
    // ... остальные опции из оригинального кода ...
};

static void list_disk_types(void)
{
	PedDiskType *type;
	int i = 0;
	printf("Disk types:\n");
	for (type = ped_disk_type_get_next(NULL); type;
		type = ped_disk_type_get_next(type)) {
		printf("%s\t", type->name);
		i++;
		if (i == 5) {
			printf("\n");
			i = 0;
		}
	}
	if (i > 0)
		printf("\n");
	printf("\n");
}

static void list_fs_types(void)
{
	PedFileSystemType *fs_type;
	int i = 0;
	printf("File system types:\n");
	for (fs_type = ped_file_system_type_get_next(NULL); fs_type;
		fs_type = ped_file_system_type_get_next(fs_type)) {
		printf("%s\t", fs_type->name);
		i++;
		if (i == 5) {
			printf("\n");
			i = 0;
		}
	}
	if (i > 0)
		printf("\n");
	printf("\n");
}

static long long arg_to_long_long(const struct argp_state *state,
	const char *arg)
{
	char *end;
	long long ll = strtoll(arg, &end, 0);
	if (!arg)
		argp_error(state, "An integer must be provided");
	if (!*arg || *end)
		argp_error(state, "`%s' is not an integer", arg);
	return ll;
}

static int fix_disk(PedDevice *dev, PedDiskType *type,
	PedFileSystemType *fs_type, int boot, PedSector start, PedSector end)
{
	PedDisk *disk;
	PedPartition *part;
	PedGeometry *geom;
	PedConstraint *constraint;
	int ret = 0;

	disk = ped_disk_new_fresh(dev, type);
	if (!disk)
		goto out;

	start = map_sector_to_logical_sector(start, dev->sector_size);
	end = map_sector_to_logical_sector(end, dev->sector_size);
	part = ped_partition_new(disk, PED_PARTITION_NORMAL,
		fs_type, start, end);
	if (!part)
		goto disk;
	if (boot && !ped_partition_set_flag(part, PED_PARTITION_BOOT, 1))
		goto part;

	geom = ped_geometry_new(dev, start, end - start + 1);
	if (!geom)
		goto part;
	constraint = ped_constraint_exact(geom);
	ped_geometry_destroy(geom);
	if (!constraint)
		goto part;

	ret = ped_disk_add_partition(disk, part, constraint);
	ped_constraint_destroy(constraint);
	if (!ret)
		goto part;
	/* ped_disk_print(disk); */

	ret = ped_disk_commit(disk);
	goto disk;

part:
	ped_partition_destroy(part);
disk:
	ped_disk_destroy(disk);
out:
	return ret;
}

static PedSector map_sector_to_logical_sector(PedSector sector,
	int logical_sector_size)
{
	assert(logical_sector_size >= 512);
	assert(logical_sector_size % 512 == 0);
	return sector / (logical_sector_size / 512);
}

// Оригинальные Linux-функции с argp
static error_t parse_opt(int key, char *arg, struct argp_state *state) {
	struct args *args = state->input;
	long long ll;

	switch (key) {
	case 'd':
		args->disk_type = ped_disk_type_get(arg);
		if (!args->disk_type)
			argp_error(state,
				"Disk type `%s' is not supported; use --list-disk-types to see the supported types",
				arg);
		break;

	case 'f':
		args->fs_type = ped_file_system_type_get(arg);
		if (!args->fs_type)
			argp_error(state,
				"File system type `%s' is not supported; use --list-fs-types to see the supported types",
				arg);
		break;

	case 'b':
		args->boot = true;
		break;

	case 'n':
		args->boot = false;
		break;

	case 'a':
		ll = arg_to_long_long(state, arg);
		if (ll < 0)
			argp_error(state,
				"First sector must be greater or equal to 0");
		args->first_sec = ll;
		break;

	case 'l':
		ll = arg_to_long_long(state, arg);
		if (ll < 0)
			argp_error(state,
				"Last sector must be greater or equal to 0");
		args->last_sec = ll;
		break;

	case 'k':
		args->list_disk_types = true;
		break;

	case 's':
		args->list_fs_types = true;
		break;

	case ARGP_KEY_INIT:
		args->dev_filename = NULL;
		args->last_sec = -1;
		break;

	case ARGP_KEY_ARG:
		if (args->dev_filename)
			argp_error(state,
				"Wrong number of arguments; only one is allowed");
		args->dev_filename = arg;
		break;

	case ARGP_KEY_END:
		if (args->list_disk_types || args->list_fs_types)
			break;

		if (!args->dev_filename)
			argp_error(state,
				"The disk device was not specified");

		if (args->last_sec < 0)
			argp_error(state,
				"Option --last-sec is required");
		if (args->first_sec > args->last_sec)
			argp_error(state,
				"Option --fist_sec must be less or equal to option --last_sec");
		break;

	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = {options, parse_opt, adoc, doc, NULL, NULL, NULL};
#endif

int main(int argc, char *argv[]) {
    struct args args = {
        .list_disk_types = false,
        .list_fs_types = false,
        .boot = true,
        .disk_type = ped_disk_type_get("msdos"),  // Исправлено
        .fs_type = ped_file_system_type_get("fat32"),  // Исправлено
        .first_sec = DEFAULT_FIRST_SEC,
        .last_sec = -1
    };

#ifdef _WIN32
    // Windows инициализация
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    parse_args(argc, argv, &args);
#else
    // Linux парсинг аргументов
    argp_parse(&argp, argc, argv, 0, NULL, &args);
#endif

    // Общая логика
    if (args.list_disk_types) {
        printf("Supported disk types: msdos\n");
        return 0;
    }

    if (args.list_fs_types) {
        printf("Supported filesystems: fat32\n");
        return 0;
    }

    if (!args.dev_filename || args.last_sec < 0) {
        fprintf(stderr, "Missing required arguments\n");
        return 1;
    }

#ifdef _WIN32
    // Windows: преобразование пути
    char win_path[MAX_PATH];
    char drive_letter;
    if (sscanf(args.dev_filename, "/dev/sd%c", &drive_letter) == 1) {
        int drive_num = tolower(drive_letter) - 'a';
        snprintf(win_path, MAX_PATH, "\\\\.\\PhysicalDrive%d", drive_num);
    } else {
        strncpy(win_path, args.dev_filename, MAX_PATH);
    }

    if (!fix_disk_win(win_path, args.first_sec, args.last_sec)) {
        fprintf(stderr, "Failed to fix disk\n");
        return 1;
    }
#else
    // Linux реализация с libparted
    PedDevice *dev = ped_device_get(args.dev_filename);
	int ret;

	/* Read parameters. */
	argp_parse(&argp, argc, argv, 0, NULL, &args);
	print_header(stdout, "fix");

	if (args.list_disk_types)
		list_disk_types();

	if (args.list_fs_types)
		list_fs_types();

	if (args.list_disk_types || args.list_fs_types) {
		/* If the user has asked for the types,
		 * she doesn't want to fix the drive yet.
		 */
		return 0;
	}
	
	if (!args.disk_type || !args.fs_type) {
	    fprintf(stderr, "Invalid disk or filesystem type\n");
	    return 1;
	}
	
	/* XXX If @dev is a partition, refer the user to
	 * the disk of this partition.
	 */
	if (!dev)
		return 1;

	ret = !fix_disk(dev, args.disk_type, args.fs_type, args.boot,
	    args.first_sec, args.last_sec);
	return ret;
#endif

    printf("Drive %s was successfully fixed\n", args.dev_filename);
    return 0;
}
