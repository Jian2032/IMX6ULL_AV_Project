/*
 * ov5640_diag.c - OV5640 DVP input diagnostic utility for VIDEO-R5
 *
 * The normal preview path has already proved that the last YUYV frame, the
 * software RGB565 conversion and the visible framebuffer contain the same
 * displaced image.  This utility moves the diagnostic boundary one stage
 * earlier by asking the sensor to generate an internal color-bar pattern.
 *
 * The program deliberately exposes only two narrowly scoped write groups:
 *
 *   pattern on      -> write 0x84: static vertical color bars
 *   pattern rolling -> write 0xc4: color bars plus a rolling horizontal bar
 *   pattern off     -> write 0x00: disable the internal pattern
 *   drive 1..4      -> update only 0x302c[7:6]: DVP output drive strength
 *
 * Arbitrary register writes are not provided.  The read-only "status" command
 * reports the registers needed to check chip identity, output size, DVP line
 * and frame timing, byte order, signal polarity and test-pattern state.
 *
 * The OV5640 is already owned by the kernel subdevice driver.  I2C_SLAVE_FORCE
 * is therefore needed for this narrowly scoped diagnostic.  Stop every V4L2
 * capture process before running this tool; never change registers while CSI
 * DMA is active.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define OV5640_I2C_ADDRESS             0x3c
#define OV5640_CHIP_ID_HIGH            0x300a
#define OV5640_CHIP_ID_LOW             0x300b
#define OV5640_OUTPUT_WIDTH_HIGH       0x3808
#define OV5640_OUTPUT_WIDTH_LOW        0x3809
#define OV5640_OUTPUT_HEIGHT_HIGH      0x380a
#define OV5640_OUTPUT_HEIGHT_LOW       0x380b
#define OV5640_HTS_HIGH                0x380c
#define OV5640_HTS_LOW                 0x380d
#define OV5640_VTS_HIGH                0x380e
#define OV5640_VTS_LOW                 0x380f
#define OV5640_FORMAT_CONTROL          0x4300
#define OV5640_DVP_POLARITY            0x4740
#define OV5640_DVP_DRIVE_CONTROL       0x302c
#define OV5640_TEST_PATTERN            0x503d

/* The NXP 4.1.15 OV5640 driver documents 0x302c[7:6] as 1x..4x. */
#define OV5640_DVP_DRIVE_MASK          0xc0
#define OV5640_DVP_DRIVE_SHIFT         6

/* 0x84 is the mainline Linux OV5640 value for vertical color bars. */
#define OV5640_TEST_PATTERN_COLOR_BARS 0x84
#define OV5640_TEST_PATTERN_ROLLING    0xc4
#define OV5640_TEST_PATTERN_DISABLED   0x00

static int i2c_transfer_exact(int fd, struct i2c_msg *messages,
			      unsigned int message_count)
{
	struct i2c_rdwr_ioctl_data transfer;
	int ret;

	memset(&transfer, 0, sizeof(transfer));
	transfer.msgs = messages;
	transfer.nmsgs = message_count;

	do {
		ret = ioctl(fd, I2C_RDWR, &transfer);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0)
		return -1;
	if ((unsigned int)ret != message_count) {
		errno = EIO;
		return -1;
	}

	return 0;
}

/*
 * OV5640 register addresses are 16 bits, while register values are 8 bits.
 * A read is one combined I2C transaction: write the address, then issue a
 * repeated START and read one byte.  This avoids another bus master changing
 * the sensor's internal address pointer between two separate syscalls.
 */
static int ov5640_read_register(int fd, uint16_t reg, uint8_t *value)
{
	uint8_t address[2];
	struct i2c_msg messages[2];

	address[0] = (uint8_t)(reg >> 8);
	address[1] = (uint8_t)(reg & 0xff);

	memset(messages, 0, sizeof(messages));
	messages[0].addr = OV5640_I2C_ADDRESS;
	messages[0].flags = 0;
	messages[0].len = sizeof(address);
	messages[0].buf = address;
	messages[1].addr = OV5640_I2C_ADDRESS;
	messages[1].flags = I2C_M_RD;
	messages[1].len = 1;
	messages[1].buf = value;

	return i2c_transfer_exact(fd, messages, 2);
}

static int ov5640_write_register(int fd, uint16_t reg, uint8_t value)
{
	uint8_t payload[3];
	struct i2c_msg message;

	payload[0] = (uint8_t)(reg >> 8);
	payload[1] = (uint8_t)(reg & 0xff);
	payload[2] = value;

	memset(&message, 0, sizeof(message));
	message.addr = OV5640_I2C_ADDRESS;
	message.flags = 0;
	message.len = sizeof(payload);
	message.buf = payload;

	return i2c_transfer_exact(fd, &message, 1);
}

static int read_u8(int fd, uint16_t reg, uint8_t *value)
{
	if (ov5640_read_register(fd, reg, value) < 0) {
		fprintf(stderr, "read register 0x%04x failed: %s\n",
			reg, strerror(errno));
		return -1;
	}

	return 0;
}

static int read_u16_pair(int fd, uint16_t high_reg, uint16_t low_reg,
			 uint16_t *value)
{
	uint8_t high;
	uint8_t low;

	if (read_u8(fd, high_reg, &high) < 0 ||
	    read_u8(fd, low_reg, &low) < 0)
		return -1;

	*value = ((uint16_t)high << 8) | low;
	return 0;
}

static const char *level_name(unsigned int high)
{
	return high ? "active-high" : "active-low";
}

static int print_status(int fd, const char *device)
{
	uint8_t id_high;
	uint8_t id_low;
	uint8_t format;
	uint8_t polarity;
	uint8_t drive;
	uint8_t pattern;
	uint16_t width;
	uint16_t height;
	uint16_t hts;
	uint16_t vts;

	if (read_u8(fd, OV5640_CHIP_ID_HIGH, &id_high) < 0 ||
	    read_u8(fd, OV5640_CHIP_ID_LOW, &id_low) < 0 ||
	    read_u16_pair(fd, OV5640_OUTPUT_WIDTH_HIGH,
			  OV5640_OUTPUT_WIDTH_LOW, &width) < 0 ||
	    read_u16_pair(fd, OV5640_OUTPUT_HEIGHT_HIGH,
			  OV5640_OUTPUT_HEIGHT_LOW, &height) < 0 ||
	    read_u16_pair(fd, OV5640_HTS_HIGH, OV5640_HTS_LOW, &hts) < 0 ||
	    read_u16_pair(fd, OV5640_VTS_HIGH, OV5640_VTS_LOW, &vts) < 0 ||
	    read_u8(fd, OV5640_FORMAT_CONTROL, &format) < 0 ||
	    read_u8(fd, OV5640_DVP_POLARITY, &polarity) < 0 ||
	    read_u8(fd, OV5640_DVP_DRIVE_CONTROL, &drive) < 0 ||
	    read_u8(fd, OV5640_TEST_PATTERN, &pattern) < 0)
		return -1;

	printf("OV5640 DVP diagnostic\n");
	printf("i2c device     : %s, slave=0x%02x\n",
	       device, OV5640_I2C_ADDRESS);
	printf("chip id        : 0x%02x%02x%s\n", id_high, id_low,
	       (id_high == 0x56 && id_low == 0x40) ? " [PASS]" : " [UNEXPECTED]");
	printf("output size    : %ux%u\n", width, height);
	printf("sensor timing  : HTS=%u VTS=%u\n", hts, vts);
	printf("format 0x4300  : 0x%02x%s\n", format,
	       format == 0x30 ? " (YUYV)" : "");
	printf("polarity 0x4740: 0x%02x\n", polarity);
	printf("  PCLK         : %s\n", level_name((polarity >> 5) & 1));
	printf("  HREF         : %s\n", level_name((polarity >> 1) & 1));
	printf("  VSYNC        : %s (datasheet naming)\n",
	       level_name(polarity & 1));
	printf("drive 0x302c   : 0x%02x (%ux)\n", drive,
	       ((drive & OV5640_DVP_DRIVE_MASK) >>
		OV5640_DVP_DRIVE_SHIFT) + 1U);
	printf("test 0x503d    : 0x%02x (%s)\n", pattern,
	       (pattern & 0x80) ? "enabled" : "disabled");

	return 0;
}

/*
 * Change only the two drive-strength bits and preserve 0x302c[5:0].
 *
 * A stronger setting is not automatically better: it can improve a slow or
 * heavily loaded edge, but too much drive can also increase ringing.  The
 * caller therefore selects one exact value, tests it, and can restore 2x.
 */
static int set_drive_strength(int fd, unsigned int strength)
{
	uint8_t before;
	uint8_t requested;
	uint8_t actual;

	if (strength < 1U || strength > 4U) {
		fprintf(stderr, "drive strength must be 1, 2, 3 or 4\n");
		errno = EINVAL;
		return -1;
	}

	if (read_u8(fd, OV5640_DVP_DRIVE_CONTROL, &before) < 0)
		return -1;

	requested = (uint8_t)(before & ~OV5640_DVP_DRIVE_MASK);
	requested |= (uint8_t)((strength - 1U) <<
			       OV5640_DVP_DRIVE_SHIFT);

	if (ov5640_write_register(fd, OV5640_DVP_DRIVE_CONTROL,
				   requested) < 0) {
		fprintf(stderr, "write register 0x%04x failed: %s\n",
			OV5640_DVP_DRIVE_CONTROL, strerror(errno));
		return -1;
	}

	/* Verify the complete byte, not only [7:6], so the test cannot silently
	 * disturb an unrelated bit in the same register. */
	if (read_u8(fd, OV5640_DVP_DRIVE_CONTROL, &actual) < 0)
		return -1;
	if (actual != requested) {
		fprintf(stderr,
			"drive readback mismatch: wrote 0x%02x, read 0x%02x\n",
			requested, actual);
		errno = EIO;
		return -1;
	}

	printf("OV5640 DVP drive: %ux, reg 0x302c: 0x%02x -> 0x%02x\n",
	       strength, before, actual);
	return 0;
}

static int set_test_pattern(int fd, uint8_t requested, const char *description)
{
	uint8_t actual;

	if (ov5640_write_register(fd, OV5640_TEST_PATTERN, requested) < 0) {
		fprintf(stderr, "write register 0x%04x failed: %s\n",
			OV5640_TEST_PATTERN, strerror(errno));
		return -1;
	}

	/* Read back immediately so a disconnected or marginal I2C bus cannot
	 * turn a failed diagnostic setup into a misleading camera result. */
	if (read_u8(fd, OV5640_TEST_PATTERN, &actual) < 0)
		return -1;
	if (actual != requested) {
		fprintf(stderr,
			"test-pattern readback mismatch: wrote 0x%02x, read 0x%02x\n",
			requested, actual);
		errno = EIO;
		return -1;
	}

	printf("OV5640 test pattern: %s, reg 0x503d=0x%02x\n",
	       description, actual);
	return 0;
}

static void print_usage(const char *program)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s [i2c-device] status\n"
		"  %s [i2c-device] pattern on|rolling|off\n"
		"  %s [i2c-device] drive 1|2|3|4\n"
		"\n"
		"Default i2c-device: /dev/i2c-1 (OV5640 appears as 1-003c)\n"
		"Stop every V4L2 capture process before changing sensor state.\n",
		program, program, program);
}

int main(int argc, char **argv)
{
	const char *device = "/dev/i2c-1";
	const char *command;
	const char *argument = NULL;
	struct stat st;
	int fd;
	int arg_index = 1;
	int ret = EXIT_FAILURE;

	if (argc > 1 && argv[1][0] == '/') {
		device = argv[1];
		arg_index++;
	}
	if (argc <= arg_index) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	command = argv[arg_index++];
	if (argc > arg_index)
		argument = argv[arg_index++];
	if (argc != arg_index) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	if (stat(device, &st) < 0) {
		fprintf(stderr, "stat %s failed: %s\n", device, strerror(errno));
		return EXIT_FAILURE;
	}
	if (!S_ISCHR(st.st_mode)) {
		fprintf(stderr, "%s is not a character device\n", device);
		return EXIT_FAILURE;
	}

	fd = open(device, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", device, strerror(errno));
		return EXIT_FAILURE;
	}

	/* The kernel OV5640 subdevice owns address 0x3c.  FORCE is intentional,
	 * tightly scoped, and safe only while no capture process is running. */
	if (ioctl(fd, I2C_SLAVE_FORCE, OV5640_I2C_ADDRESS) < 0) {
		fprintf(stderr, "select OV5640 address 0x%02x failed: %s\n",
			OV5640_I2C_ADDRESS, strerror(errno));
		goto out_close;
	}

	if (strcmp(command, "status") == 0 && argument == NULL) {
		ret = print_status(fd, device) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	} else if (strcmp(command, "pattern") == 0 && argument != NULL) {
		if (strcmp(argument, "on") == 0)
			ret = set_test_pattern(fd, OV5640_TEST_PATTERN_COLOR_BARS,
					       "static color bars") == 0 ?
			      EXIT_SUCCESS : EXIT_FAILURE;
		else if (strcmp(argument, "rolling") == 0)
			ret = set_test_pattern(fd, OV5640_TEST_PATTERN_ROLLING,
					       "color bars with rolling bar") == 0 ?
			      EXIT_SUCCESS : EXIT_FAILURE;
		else if (strcmp(argument, "off") == 0)
			ret = set_test_pattern(fd, OV5640_TEST_PATTERN_DISABLED,
					       "disabled") == 0 ?
			      EXIT_SUCCESS : EXIT_FAILURE;
		else
			print_usage(argv[0]);
	} else if (strcmp(command, "drive") == 0 && argument != NULL) {
		char *end = NULL;
		unsigned long strength;

		errno = 0;
		strength = strtoul(argument, &end, 10);
		if (errno != 0 || end == argument || *end != '\0' ||
		    strength < 1UL || strength > 4UL) {
			fprintf(stderr, "drive strength must be 1, 2, 3 or 4\n");
		} else {
			ret = set_drive_strength(fd, (unsigned int)strength) == 0 ?
			      EXIT_SUCCESS : EXIT_FAILURE;
		}
	} else {
		print_usage(argv[0]);
	}

out_close:
	if (close(fd) < 0 && ret == EXIT_SUCCESS) {
		fprintf(stderr, "close %s failed: %s\n", device, strerror(errno));
		ret = EXIT_FAILURE;
	}
	return ret;
}
