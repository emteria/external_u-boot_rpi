// SPDX-License-Identifier: GPL-2.0+
/*
 * Boot an Android A/B image with verified boot.
 *
 * Copyright (c) 2026 Emteria
 */

#define LOG_CATEGORY LOGC_BOOT

#include <android_ab.h>
#include <android_image.h>
#include <avb_verify.h>
#include <bcb.h>
#include <blk.h>
#include <bootm.h>
#include <command.h>
#include <dm.h>
#include <env.h>
#include <image.h>
#include <log.h>
#include <mapmem.h>
#include <part.h>
#include <stdarg.h>
#include <vsprintf.h>
#include <asm/global_data.h>
#include <dm/uclass.h>
#include <linux/libfdt.h>
#include <linux/string.h>

DECLARE_GLOBAL_DATA_PTR;

/* One Android page holds the header, which carries the real image size */
#define BOOTANDROID_HDR_SIZE		ANDR_GKI_PAGE_SIZE

/* Largest page size we accept in a header, to keep the size arithmetic sane */
#define BOOTANDROID_PAGE_MAX		0x10000

/* Matches COMMAND_LINE_SIZE on arm64; the kernel truncates anything longer */
#define BOOTANDROID_ARGS_MAX		2048

/* One argument, sized for the longest sysfs path a Pi publishes */
#define BOOTANDROID_ARG_MAX		128

/* "0x", two hex digits per address byte, NUL */
#define BOOTANDROID_ADDR_LEN		(2 + 2 * sizeof(ulong) + 1)

/* What userspace asked for on the previous boot */
enum bootandroid_mode {
	BOOTANDROID_MODE_NORMAL,
	BOOTANDROID_MODE_RECOVERY,
	BOOTANDROID_MODE_BOOTLOADER,
};

enum bootandroid_img {
	BOOTANDROID_IMG_BOOT,
	BOOTANDROID_IMG_VENDOR_BOOT,
	BOOTANDROID_IMG_COUNT,
};

/* Where one image is staged, and how much room it has there */
struct bootandroid_window {
	ulong addr;
	u64 max;
};

/* Everything the boot needs, resolved before any image is read */
struct bootandroid_ctx {
	struct blk_desc *desc;
	struct disk_partition misc;
	struct bootandroid_window win[BOOTANDROID_IMG_COUNT];
	enum bootandroid_mode mode;
	char suffix[3];
};

static int bootandroid_add_arg(char *args, size_t size, const char *arg)
{
	if (args[0] && strlcat(args, " ", size) >= size)
		return -E2BIG;

	if (strlcat(args, arg, size) >= size)
		return -E2BIG;

	return 0;
}

/* Refuse a truncated argument: Android would look for the wrong thing */
static int bootandroid_add_argf(char *args, size_t size, const char *fmt, ...)
{
	char arg[BOOTANDROID_ARG_MAX];
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(arg, sizeof(arg), fmt, ap);
	va_end(ap);

	if (len < 0 || len >= (int)sizeof(arg))
		return -E2BIG;

	return bootandroid_add_arg(args, size, arg);
}

/* Start from the command line the firmware put in its own device tree */
static int bootandroid_base_args(char *args, size_t size)
{
	const char *fw_args;
	int node, len;

	args[0] = '\0';

	node = fdt_path_offset(gd->fdt_blob, "/chosen");
	if (node < 0)
		return 0;

	fw_args = fdt_getprop(gd->fdt_blob, node, "bootargs", &len);
	if (fw_args && len > 0 && strlcpy(args, fw_args, size) >= size)
		return -E2BIG;

	return 0;
}

/* Assemble the command line Android needs on top of the firmware's own */
static int bootandroid_build_args(const struct bootandroid_ctx *ctx,
				  char *args, size_t size)
{
	const char *boot_devices;
	const char *serial;
	int ret;

	ret = bootandroid_base_args(args, size);
	if (ret)
		return ret;

	ret = bootandroid_add_argf(args, size, "androidboot.slot_suffix=%s",
				   ctx->suffix);
	if (ret)
		return ret;

	serial = env_get("serial#");
	if (serial) {
		ret = bootandroid_add_argf(args, size,
					   "androidboot.serialno=%s", serial);
		if (ret)
			return ret;
	}

	/* We boot the firmware tree, but Android still expects the index */
	ret = bootandroid_add_arg(args, size, "androidboot.dtbo_idx=0");
	if (ret)
		return ret;

	/* Board code names the sysfs path Android should look for us on */
	boot_devices = env_get("android_boot_devices");
	if (boot_devices) {
		ret = bootandroid_add_argf(args, size,
					   "androidboot.boot_devices=%s",
					   boot_devices);
		if (ret)
			return ret;
	}

	if (ctx->mode == BOOTANDROID_MODE_NORMAL)
		return bootandroid_add_arg(args, size,
					   "androidboot.force_normal_boot=1");

	return 0;
}

/* This one only reads the header, so it is safe before verification */
static u64 bootandroid_get_boot_size(const void *hdr)
{
	const struct andr_boot_img_hdr_v0 *bhdr = hdr;
	u32 size;

	if (!is_android_boot_image_header(hdr))
		return 0;

	/* Before v3 the ramdisk and the dtb do not live in vendor_boot */
	switch (bhdr->header_version) {
	case 3:
	case 4:
		break;
	default:
		printf("bootandroid: boot header version %u is not supported\n",
		       bhdr->header_version);
		return 0;
	}

	if (!android_image_get_bootimg_size(hdr, &size))
		return 0;

	return size;
}

/* Not android_image_get_vendor_bootimg_size(): it writes a bootconfig trailer */
static u64 bootandroid_get_vendor_boot_size(const void *hdr)
{
	const struct andr_vnd_boot_img_hdr *vhdr = hdr;
	u64 page, size;

	if (!is_android_vendor_boot_image_header(vhdr))
		return 0;

	/* vendor_boot starts at v3, and v4 added the ramdisk table */
	switch (vhdr->header_version) {
	case 3:
		size = ANDR_VENDOR_BOOT_V3_SIZE;
		break;
	case 4:
		size = ANDR_VENDOR_BOOT_V4_SIZE;
		break;
	default:
		printf("bootandroid: vendor_boot header version %u is not supported\n",
		       vhdr->header_version);
		return 0;
	}

	page = vhdr->page_size;
	if (!page || page > BOOTANDROID_PAGE_MAX)
		return 0;

	size = ALIGN(size, page);
	size += ALIGN(vhdr->vendor_ramdisk_size, page);
	size += ALIGN(vhdr->dtb_size, page);

	if (vhdr->header_version >= 4) {
		size += ALIGN(vhdr->vendor_ramdisk_table_size, page);
		size += ALIGN(vhdr->bootconfig_size, page);
	}

	return size;
}

static const struct bootandroid_image {
	const char *part_prefix;
	const char *addr_env;
	ulong addr_default;
	u64 (*get_size)(const void *hdr);
} bootandroid_images[BOOTANDROID_IMG_COUNT] = {
	[BOOTANDROID_IMG_BOOT] = {
		"boot", "android_boot_addr",
		CONFIG_BOOTANDROID_BOOT_ADDR, bootandroid_get_boot_size,
	},
	[BOOTANDROID_IMG_VENDOR_BOOT] = {
		"vendor_boot", "android_vendor_boot_addr",
		CONFIG_BOOTANDROID_VENDOR_BOOT_ADDR,
		bootandroid_get_vendor_boot_size,
	},
};

/* Kconfig and the environment own the layout, since cmd/ knows no board */
static int bootandroid_get_windows(struct bootandroid_window *win)
{
	struct bootandroid_window *boot = &win[BOOTANDROID_IMG_BOOT];
	struct bootandroid_window *vendor = &win[BOOTANDROID_IMG_VENDOR_BOOT];
	int i;

	for (i = 0; i < BOOTANDROID_IMG_COUNT; i++)
		win[i].addr = env_get_hex(bootandroid_images[i].addr_env,
					  bootandroid_images[i].addr_default);

	/* The boot image window ends where vendor_boot starts */
	if (vendor->addr <= boot->addr) {
		printf("bootandroid: vendor_boot must be staged above boot\n");
		return -EINVAL;
	}

	boot->max = vendor->addr - boot->addr;
	vendor->max = CONFIG_BOOTANDROID_VENDOR_BOOT_MAX;

	for (i = 0; i < BOOTANDROID_IMG_COUNT; i++) {
		if (win[i].addr + win[i].max > gd->ram_top) {
			printf("bootandroid: '%s' is staged past the end of RAM\n",
			       bootandroid_images[i].part_prefix);
			return -EINVAL;
		}
	}

	return 0;
}

static int bootandroid_read(struct blk_desc *desc,
			    struct disk_partition *info, const char *name,
			    const struct bootandroid_window *win, u64 bytes)
{
	lbaint_t blks = DIV_ROUND_UP(bytes, info->blksz);
	u64 mapped = (u64)blks * info->blksz;
	void *buf;
	ulong n;

	if (blks > info->size) {
		printf("bootandroid: '%s' is smaller than %llu bytes\n",
		       name, bytes);
		return -EIO;
	}

	/* blk_dread writes whole blocks, so the mapping must cover them all */
	if (mapped > win->max) {
		printf("bootandroid: '%s' does not fit in its staging window\n",
		       name);
		return -EFBIG;
	}

	buf = map_sysmem(win->addr, mapped);
	n = blk_dread(desc, info->start, blks, buf);
	unmap_sysmem(buf);

	if (n != blks) {
		printf("bootandroid: short read on '%s'\n", name);
		return -EIO;
	}

	return 0;
}

/* Read the header first, so only the image itself is pulled off the disk */
static int bootandroid_load_image(struct blk_desc *desc, const char *suffix,
				  enum bootandroid_img which,
				  const struct bootandroid_window *win)
{
	const struct bootandroid_image *img = &bootandroid_images[which];
	struct disk_partition info;
	char name[PART_NAME_LEN];
	void *hdr;
	u64 size;
	int ret;

	snprintf(name, sizeof(name), "%s%s", img->part_prefix, suffix);

	if (part_get_info_by_name(desc, name, &info) < 0) {
		printf("bootandroid: no '%s' partition\n", name);
		return -ENOENT;
	}

	ret = bootandroid_read(desc, &info, name, win, BOOTANDROID_HDR_SIZE);
	if (ret)
		return ret;

	hdr = map_sysmem(win->addr, BOOTANDROID_HDR_SIZE);
	size = img->get_size(hdr);
	unmap_sysmem(hdr);

	if (!size || size > (u64)info.size * info.blksz) {
		printf("bootandroid: '%s' has an unusable size of %llu bytes\n",
		       name, size);
		return -EFBIG;
	}

	return bootandroid_read(desc, &info, name, win, size);
}

/* The firmware names only the medium, so find the disk that carries Android */
static int bootandroid_find_device(const char *devtype,
				   struct blk_desc **descp,
				   struct disk_partition *misc)
{
	enum uclass_id uclass_id = uclass_get_by_name(devtype);
	struct blk_desc *desc;
	struct udevice *dev;
	int devnum, ret;

	if (uclass_id == UCLASS_INVALID) {
		printf("bootandroid: '%s' is not a storage interface\n",
		       devtype);
		return -ENODEV;
	}

	/* A devnum in the environment pins the disk, for locked builds */
	if (env_get("devnum")) {
		devnum = env_get_ulong("devnum", 10, 0);

		desc = blk_get_devnum_by_uclass_id(uclass_id, devnum);
		if (!desc || part_get_info_by_name(desc, "misc", misc) < 0) {
			printf("bootandroid: %s %d carries no 'misc' partition\n",
			       devtype, devnum);
			return -ENOENT;
		}

		*descp = desc;

		return 0;
	}

	/* Otherwise any disk on that medium may present the misc partition */
	for (ret = blk_first_device(uclass_id, &dev); !ret;
	     ret = blk_next_device(&dev)) {
		desc = dev_get_uclass_plat(dev);

		if (part_get_info_by_name(desc, "misc", misc) < 0)
			continue;

		printf("bootandroid: %s %d carries 'misc', using it\n",
		       devtype, desc->devnum);
		*descp = desc;

		return 0;
	}

	printf("bootandroid: no %s device with a 'misc' partition\n", devtype);

	return -ENOENT;
}

/* bcb_load keeps the pointer, so misc must outlive every other bcb call */
static enum bootandroid_mode bootandroid_get_bcb_mode(struct blk_desc *desc,
						      struct disk_partition *misc)
{
	char command[32];

	if (bcb_load(desc, misc)) {
		log_debug("can't load the BCB\n");
		return BOOTANDROID_MODE_NORMAL;
	}

	if (bcb_get(BCB_FIELD_COMMAND, command, sizeof(command))) {
		log_debug("can't read the BCB command\n");
		return BOOTANDROID_MODE_NORMAL;
	}

	if (!strcmp(command, "bootonce-bootloader"))
		return BOOTANDROID_MODE_BOOTLOADER;

	/* fastbootd runs inside recovery, which reads the command itself */
	if (!strcmp(command, "boot-fastboot") ||
	    !strcmp(command, "boot-recovery"))
		return BOOTANDROID_MODE_RECOVERY;

	return BOOTANDROID_MODE_NORMAL;
}

/* "bootonce" means the request must not survive this boot */
static int bootandroid_clear_bcb(void)
{
	int ret;

	ret = bcb_set(BCB_FIELD_COMMAND, "");
	if (ret)
		return ret;

	return bcb_store();
}

/* USB, then TCP, then UDP: a board rarely builds more than one of them */
static int bootandroid_enter_fastboot(void)
{
	char cmd[32];

	if (IS_ENABLED(CONFIG_USB_FUNCTION_FASTBOOT)) {
		snprintf(cmd, sizeof(cmd), "fastboot usb %ld",
			 IF_ENABLED_INT(CONFIG_USB_FUNCTION_FASTBOOT,
					CONFIG_FASTBOOT_USB_DEV));
		return run_command(cmd, 0);
	}

	if (IS_ENABLED(CONFIG_TCP_FUNCTION_FASTBOOT))
		return run_command("fastboot tcp", 0);

	if (IS_ENABLED(CONFIG_UDP_FUNCTION_FASTBOOT))
		return run_command("fastboot udp", 0);

	printf("bootandroid: no fastboot transport is configured\n");

	return -EOPNOTSUPP;
}

/* Where a board with nothing bootable ends up, rather than at a prompt */
static int bootandroid_run_fastboot(bool bootonce)
{
	/* Never honour a one-shot request we are unable to retract */
	if (bootonce && bootandroid_clear_bcb()) {
		printf("bootandroid: can't clear the BCB command, refusing\n");
		return CMD_RET_FAILURE;
	}

	printf("bootandroid: entering fastboot\n");
	bootandroid_enter_fastboot();
	do_reset(NULL, 0, 0, NULL);

	return CMD_RET_FAILURE;
}

static int bootandroid_verify(struct blk_desc *desc, const char *slot_suffix,
			      char *args, size_t size)
{
	const char * const parts[] = {"boot", "vendor_boot", NULL};
	AvbSlotVerifyData *out_data = NULL;
	AvbSlotVerifyResult result;
	bool unlocked = false;
	AvbOps *ops;
	char *state_arg;
	int ret;

	/* run_avb_verification() in boot/bootmeth_android.c is the sibling */
	ops = avb_ops_alloc(desc->uclass_id, desc->devnum);
	if (!ops) {
		printf("bootandroid: can't allocate AvbOps\n");
		return -ENOMEM;
	}

	if (ops->read_is_device_unlocked(ops, &unlocked) != AVB_IO_RESULT_OK) {
		printf("bootandroid: can't read the device lock state\n");
		ret = -EIO;
		goto out;
	}

	result = avb_slot_verify(ops, parts, slot_suffix,
				 unlocked ?
				 AVB_SLOT_VERIFY_FLAGS_ALLOW_VERIFICATION_ERROR :
				 AVB_SLOT_VERIFY_FLAGS_NONE,
				 AVB_HASHTREE_ERROR_MODE_RESTART_AND_INVALIDATE,
				 &out_data);

	/* An unlocked device is allowed to run unverified images */
	if (unlocked && result == AVB_SLOT_VERIFY_RESULT_ERROR_VERIFICATION)
		result = AVB_SLOT_VERIFY_RESULT_OK;

	if (result != AVB_SLOT_VERIFY_RESULT_OK) {
		printf("bootandroid: AVB failed: %s\n",
		       str_avb_slot_error(result));
		ret = -EPERM;
		goto out;
	}

	if (out_data && out_data->cmdline) {
		ret = bootandroid_add_arg(args, size, out_data->cmdline);
		if (ret)
			goto out;
	}

	/* avb_set_state returns a literal, so there is nothing to free */
	state_arg = avb_set_state(ops, unlocked ? AVB_ORANGE : AVB_GREEN);
	if (state_arg) {
		ret = bootandroid_add_arg(args, size, state_arg);
		if (ret)
			goto out;
	}

	ret = 0;
out:
	if (out_data)
		avb_slot_verify_data_free(out_data);
	avb_ops_free(ops);

	return ret;
}

static int bootandroid_resolve(struct bootandroid_ctx *ctx)
{
	const char *devtype;
	int ret;

	/* Board code publishes the medium the firmware actually booted from */
	devtype = env_get("devtype");
	if (!devtype) {
		printf("bootandroid: the board published no boot device\n");
		return -ENODEV;
	}

	ret = bootandroid_get_windows(ctx->win);
	if (ret)
		return ret;

	ret = bootandroid_find_device(devtype, &ctx->desc, &ctx->misc);
	if (ret)
		return ret;

	ctx->mode = bootandroid_get_bcb_mode(ctx->desc, &ctx->misc);

	return 0;
}

static int bootandroid_boot(const struct bootandroid_ctx *ctx,
			    const char *args)
{
	char kaddr[BOOTANDROID_ADDR_LEN], faddr[BOOTANDROID_ADDR_LEN];
	struct bootm_info bmi;

	env_set("bootargs", args);

	snprintf(kaddr, sizeof(kaddr), "0x%lx",
		 ctx->win[BOOTANDROID_IMG_BOOT].addr);
	snprintf(faddr, sizeof(faddr), "0x%lx",
		 (ulong)map_to_sysmem(gd->fdt_blob));

	bootm_init(&bmi);
	bmi.addr_img = kaddr;
	/* The ramdisk lives inside the boot image, so bootm takes that address */
	bmi.conf_ramdisk = kaddr;
	bmi.conf_fdt = faddr;

	/* BOOTM_STATE_FDT relocates the firmware tree into the boot map */
	/* boot_run adds MEASURE, OS_PREP, OS_FAKE_GO, OS_GO and the ramdisk */
	return boot_run(&bmi, "bootandroid",
			BOOTM_STATE_START | BOOTM_STATE_FINDOS |
			BOOTM_STATE_PRE_LOAD | BOOTM_STATE_FINDOTHER |
			BOOTM_STATE_LOADOS | BOOTM_STATE_FDT);
}

static int do_bootandroid(struct cmd_tbl *cmdtp, int flag, int argc,
			  char *const argv[])
{
	char args[BOOTANDROID_ARGS_MAX];
	struct bootandroid_ctx ctx;
	int i, slot, ret;

	if (bootandroid_resolve(&ctx))
		return CMD_RET_FAILURE;

	if (ctx.mode == BOOTANDROID_MODE_BOOTLOADER)
		return bootandroid_run_fastboot(true);

	/* Marks the slot as tried, so a failing one runs out of attempts */
	slot = ab_select_slot(ctx.desc, &ctx.misc, true);
	if (slot < 0) {
		printf("bootandroid: no bootable slot\n");
		return bootandroid_run_fastboot(false);
	}
	snprintf(ctx.suffix, sizeof(ctx.suffix), "_%c", BOOT_SLOT_NAME(slot));

	for (i = 0; i < BOOTANDROID_IMG_COUNT; i++) {
		if (bootandroid_load_image(ctx.desc, ctx.suffix, i,
					   &ctx.win[i]))
			return CMD_RET_FAILURE;
	}

	set_abootimg_addr(ctx.win[BOOTANDROID_IMG_BOOT].addr);
	set_avendor_bootimg_addr(ctx.win[BOOTANDROID_IMG_VENDOR_BOOT].addr);

	if (ctx.mode == BOOTANDROID_MODE_RECOVERY)
		printf("bootandroid: booting recovery\n");

	if (bootandroid_build_args(&ctx, args, sizeof(args))) {
		printf("bootandroid: the command line needs more than %d bytes\n",
		       BOOTANDROID_ARGS_MAX);
		return CMD_RET_FAILURE;
	}

	/* Recovery ships in the same boot image, so it is verified as well */
	ret = bootandroid_verify(ctx.desc, ctx.suffix, args, sizeof(args));

	/* A full buffer is not a verification failure; a reset would not fix it */
	if (ret == -E2BIG) {
		printf("bootandroid: the command line needs more than %d bytes\n",
		       BOOTANDROID_ARGS_MAX);
		return CMD_RET_FAILURE;
	}

	if (ret) {
		printf("bootandroid: verification failed, resetting\n");
		do_reset(NULL, 0, 0, NULL);
		return CMD_RET_FAILURE;
	}

	return bootandroid_boot(&ctx, args);
}

U_BOOT_CMD(
	bootandroid, 1, 0, do_bootandroid,
	"boot an Android A/B image with verified boot",
	"\n"
	"    Selects the A/B slot, loads boot and vendor_boot from the medium\n"
	"    in ${devtype}, runs AVB, and boots. ${devnum} pins one device;\n"
	"    without it, every device on that medium is searched for a 'misc'\n"
	"    partition. The BCB can ask for recovery or for fastboot instead,\n"
	"    and a board with no bootable slot enters fastboot.\n"
	"    AVB runs on its own operations, not on those of 'avb init'."
);
