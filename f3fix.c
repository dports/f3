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
    const char *disk_type;
    const char *fs_type;
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
        .disk_type = "msdos",
        .fs_type = "fat32",
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

	/* XXX If @dev is a partition, refer the user to
	 * the disk of this partition.
	 */
	dev = ped_device_get(args.dev_filename);
	if (!dev)
		return 1;

	ret = !fix_disk(dev, args.disk_type, args.fs_type, args.boot,
		args.first_sec, args.last_sec);
#endif

    printf("Drive %s was successfully fixed\n", args.dev_filename);
    return 0;
}
