/*    $Alcatel-Lucent: autoboot.c,v 1.0.1 2011/06/16 20:58:30 binl Exp $    */

/*
 * Copyright (C) 2006 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* 2011-01-05: psBase hardcode to the ALU 9375 SCS file system */

FILE_LICENCE ( GPL2_OR_LATER );

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <ipxe/netdevice.h>
#include <ipxe/dhcp.h>
#include <ipxe/settings.h>
#include <ipxe/image.h>
#include <ipxe/sanboot.h>
#include <ipxe/uri.h>
#include <ipxe/open.h>
#include <ipxe/init.h>
#include <usr/ifmgmt.h>
#include <usr/route.h>
#include <usr/dhcpmgmt.h>
#include <usr/imgmgmt.h>
#include <usr/autoboot.h>

#define ALU_MOD
#ifdef ALU_MOD
#include <ipxe/io.h> /* outb, get_memmap */

#define PIIX4_RESET_PORT	0xcf9
#define PIIX4_RESET_VAL		0x6

#define BOOT_IMAGE "view/bootView/scs/cbBootImage"

/* reset system via ich */
void rst_via_ich_to_bios(void)
{
	/*
	 * machine restart after this register is poked on the PIIX4
	 */
	outb(PIIX4_RESET_VAL, PIIX4_RESET_PORT);
}

#endif /* ALU_MOD */

/** @file
 *
 * Automatic booting
 *
 */

/* Disambiguate the various error causes */
#define ENOENT_BOOT __einfo_error ( EINFO_ENOENT_BOOT )
#define EINFO_ENOENT_BOOT \
	__einfo_uniqify ( EINFO_ENOENT, 0x01, "Nothing to boot" )

/**
 * Perform PXE menu boot when PXE stack is not available
 */
__weak int pxe_menu_boot ( struct net_device *netdev __unused ) {
	return -ENOTSUP;
}

/**
 * Identify the boot network device
 *
 * @ret netdev		Boot network device
 */
static struct net_device * find_boot_netdev ( void ) {
	return NULL;
}

/**
 * Parse next-server and filename into a URI
 *
 * @v next_server	Next-server address
 * @v filename		Filename
 * @ret uri		URI, or NULL on failure
 */
static struct uri * parse_next_server_and_filename ( struct in_addr next_server,
						     const char *filename ) {
	char buf[ 23 /* "tftp://xxx.xxx.xxx.xxx/" */ + strlen ( filename )
		  + 1 /* NUL */ ];
	struct uri *uri;

	/* Parse filename */
	uri = parse_uri ( filename );
	if ( ! uri )
		return NULL;

	/* Construct a tftp:// URI for the filename, if applicable.
	 * We can't just rely on the current working URI, because the
	 * relative URI resolution will remove the distinction between
	 * filenames with and without initial slashes, which is
	 * significant for TFTP.
	 */
	if ( next_server.s_addr && filename[0] && ! uri_is_absolute ( uri ) ) {
		uri_put ( uri );
		snprintf ( buf, sizeof ( buf ), "tftp://%s/%s",
			   inet_ntoa ( next_server ), filename );
		uri = parse_uri ( buf );
		if ( ! uri )
			return NULL;
	}

	return uri;
}

/** The "keep-san" setting */
struct setting keep_san_setting __setting ( SETTING_SANBOOT_EXTRA ) = {
	.name = "keep-san",
	.description = "Preserve SAN connection",
	.tag = DHCP_EB_KEEP_SAN,
	.type = &setting_type_int8,
};

/** The "skip-san-boot" setting */
struct setting skip_san_boot_setting __setting ( SETTING_SANBOOT_EXTRA ) = {
	.name = "skip-san-boot",
	.description = "Do not boot from SAN device",
	.tag = DHCP_EB_SKIP_SAN_BOOT,
	.type = &setting_type_int8,
};

/**
 * Boot from filename and root-path URIs
 *
 * @v filename		Filename
 * @v root_path		Root path
 * @v drive		SAN drive (if applicable)
 * @v flags		Boot action flags
 * @ret rc		Return status code
 *
 * The somewhat tortuous flow of control in this function exists in
 * order to ensure that the "sanboot" command remains identical in
 * function to a SAN boot via a DHCP-specified root path, and to
 * provide backwards compatibility for the "keep-san" and
 * "skip-san-boot" options.
 */
int uriboot ( struct uri *filename, struct uri *root_path, int drive,
	      unsigned int flags ) {
	int rc;

	/* Hook SAN device, if applicable */
	if ( root_path ) {
		if ( ( rc = san_hook ( root_path, drive ) ) != 0 ) {
			printf ( "Could not open SAN device: %s\n",
				 strerror ( rc ) );
			goto err_san_hook;
		}
		printf ( "Registered SAN device %#02x\n", drive );
	}

	/* Describe SAN device, if applicable */
	if ( ( drive >= 0 ) && ! ( flags & URIBOOT_NO_SAN_DESCRIBE ) ) {
		if ( ( rc = san_describe ( drive ) ) != 0 ) {
			printf ( "Could not describe SAN device %#02x: %s\n",
				 drive, strerror ( rc ) );
			goto err_san_describe;
		}
	}

	/* Allow a root-path-only boot with skip-san enabled to succeed */
	rc = 0;

	/* Attempt filename boot if applicable */
	if ( filename ) {
		if ( ( rc = imgdownload ( filename, NULL, NULL,
					  register_and_boot_image ) ) != 0 ) {
			printf ( "\nCould not chain image: %s\n",
				 strerror ( rc ) );
			/* Fall through to (possibly) attempt a SAN boot
			 * as a fallback.  If no SAN boot is attempted,
			 * our status will become the return status.
			 */
		} else {
			/* Always print an extra newline, because we
			 * don't know where the NBP may have left the
			 * cursor.
			 */
			printf ( "\n" );
		}
	}

	/* Attempt SAN boot if applicable */
	if ( ( drive >= 0 ) && ! ( flags & URIBOOT_NO_SAN_BOOT ) ) {
		if ( fetch_intz_setting ( NULL, &skip_san_boot_setting) == 0 ) {
			printf ( "Booting from SAN device %#02x\n", drive );
			rc = san_boot ( drive );
			printf ( "Boot from SAN device %#02x failed: %s\n",
				 drive, strerror ( rc ) );
		} else {
			printf ( "Skipping boot from SAN device %#02x\n",
				 drive );
			/* Avoid overwriting a possible failure status
			 * from a filename boot.
			 */
		}
	}

 err_san_describe:
	/* Unhook SAN device, if applicable */
	if ( ( drive >= 0 ) && ! ( flags & URIBOOT_NO_SAN_UNHOOK ) ) {
		if ( fetch_intz_setting ( NULL, &keep_san_setting ) == 0 ) {
			san_unhook ( drive );
			printf ( "Unregistered SAN device %#02x\n", drive );
		} else {
			printf ( "Preserving SAN device %#02x\n", drive );
		}
	}
 err_san_hook:
	return rc;
}

#ifdef ALU_MOD

static void memReport(void) {
        static int validInfo = 0;
        static struct memory_map memmap;
        int rc = -ENOENT;

        if ( 0 == validInfo) {
                get_memmap ( &memmap );
                validInfo = 1;
        }

        printf("memory map in the system:\n"
              "idx:    start         end\n");
        for ( rc = 0 ; rc < (int)(memmap.count) ; rc++ ) {
                printf("%2d  0x%08x-%08x [0x%08x-%08x]\n",
                        rc,
                        (int)(memmap.regions[rc].start >> 32),
                        (int)(memmap.regions[rc].start),
                        (int)(memmap.regions[rc].end >> 32),
                        (int)(memmap.regions[rc].end) );
        }
        printf("\n");
} 

/**
 * Boot from a network device and re-try a few times before giving up
 *
 * @v netdev		Network device
 * @ret rc		Return status code
 */
static int netboot_withRetry ( struct net_device *netdev ) {
        int cntRetry = 0;
        const int maxRetry_c = 3;
        int rc = -ENOENT;

	memReport();

	for( cntRetry = 0; cntRetry < maxRetry_c; cntRetry++ )
	{
		rc = netboot(netdev);
                if ( 0 == rc ) {
			return rc;
                } else if ( (-ETIMEDOUT) == rc || (-ENOMEM) == rc ) {
                        /* no point to re-try, reboot to BIOS */
                        rst_via_ich_to_bios();
                } else {
			ifclose( netdev);
                        printf("Booting failed %d, retry %d times\n", rc, cntRetry);
                }
	}

        printf("Booting has been re-tried %d times\n", cntRetry);
	return rc;
}

#endif /* ALU_MOD */

/**
 * Close all open net devices
 *
 * Called before a fresh boot attempt in order to free up memory.  We
 * don't just close the device immediately after the boot fails,
 * because there may still be TCP connections in the process of
 * closing.
 */
static void close_all_netdevs ( void ) {
	struct net_device *netdev;

	for_each_netdev ( netdev ) {
		ifclose ( netdev );
	}
}

/**
 * Fetch next-server and filename settings into a URI
 *
 * @v settings		Settings block
 * @ret uri		URI, or NULL on failure
 */
struct uri * fetch_next_server_and_filename ( struct settings *settings ) {
	struct in_addr next_server;
	char buf[256];
	char *filename;
	struct uri *uri;

	/* Fetch next-server setting */
	fetch_ipv4_setting ( settings, &next_server_setting, &next_server );
	if ( next_server.s_addr )
		printf ( "Next server: %s\n", inet_ntoa ( next_server ) );

	/* Fetch filename setting */
	fetch_string_setting ( settings, &filename_setting,
			       buf, sizeof ( buf ) );

        #ifdef ALU_MOD
        /* 
        DHCP returns netBootLdr file name will be "view/bootView/scs/netBootLdr"
        CB boot image file name is "view/bootView/scs/cbBootImage"
        The CB bootImage is one char longer
         */
	snprintf(buf, sizeof(buf), "%s", BOOT_IMAGE);
        #endif

	if ( buf[0] )
		printf ( "Filename: %s\n", buf );

	/* Expand filename setting */
	filename = expand_settings ( buf );
	if ( ! filename )
		return NULL;

	/* Parse next server and filename */
	uri = parse_next_server_and_filename ( next_server, filename );

	free ( filename );
	return uri;
}

/**
 * Fetch root-path setting into a URI
 *
 * @v settings		Settings block
 * @ret uri		URI, or NULL on failure
 */
static struct uri * fetch_root_path ( struct settings *settings ) {
	char buf[256];
	char *root_path;
	struct uri *uri;

	/* Fetch root-path setting */
	fetch_string_setting ( settings, &root_path_setting,
			       buf, sizeof ( buf ) );
	if ( buf[0] )
		printf ( "Root path: %s\n", buf );

	/* Expand filename setting */
	root_path = expand_settings ( buf );
	if ( ! root_path )
		return NULL;

	/* Parse root path */
	uri = parse_uri ( root_path );

	free ( root_path );
	return uri;
}

/**
 * Check whether or not we have a usable PXE menu
 *
 * @ret have_menu	A usable PXE menu is present
 */
static int have_pxe_menu ( void ) {
	struct setting vendor_class_id_setting
		= { .tag = DHCP_VENDOR_CLASS_ID };
	struct setting pxe_discovery_control_setting
		= { .tag = DHCP_PXE_DISCOVERY_CONTROL };
	struct setting pxe_boot_menu_setting
		= { .tag = DHCP_PXE_BOOT_MENU };
	char buf[256];
	unsigned int pxe_discovery_control;

	fetch_string_setting ( NULL, &vendor_class_id_setting,
			       buf, sizeof ( buf ) );
	pxe_discovery_control =
		fetch_uintz_setting ( NULL, &pxe_discovery_control_setting );

	return ( ( strcmp ( buf, "PXEClient" ) == 0 ) &&
		 setting_exists ( NULL, &pxe_boot_menu_setting ) &&
		 ( ! ( ( pxe_discovery_control & PXEBS_SKIP ) &&
		       setting_exists ( NULL, &filename_setting ) ) ) );
}

/**
 * Boot from a network device
 *
 * @v netdev		Network device
 * @ret rc		Return status code
 */
int netboot ( struct net_device *netdev ) {
	struct uri *filename;
	struct uri *root_path;
	int rc;

	/* Close all other network devices */
	close_all_netdevs();

	/* Open device and display device status */
	if ( ( rc = ifopen ( netdev ) ) != 0 )
		goto err_ifopen;
	ifstat ( netdev );

	/* Configure device via DHCP */
	if ( ( rc = dhcp ( netdev ) ) != 0 )
		goto err_dhcp;
	route();

	/* Try PXE menu boot, if applicable */
	if ( have_pxe_menu() ) {
		printf ( "Booting from PXE menu\n" );
		rc = pxe_menu_boot ( netdev );
		goto err_pxe_menu_boot;
	}

	/* Fetch next server and filename */
	filename = fetch_next_server_and_filename ( NULL );
	if ( ! filename )
		goto err_filename;
	if ( ! uri_has_path ( filename ) ) {
		/* Ignore empty filename */
		uri_put ( filename );
		filename = NULL;
	}

	/* Fetch root path */
	root_path = fetch_root_path ( NULL );
	if ( ! root_path )
		goto err_root_path;
	if ( ! uri_is_absolute ( root_path ) ) {
		/* Ignore empty root path */
		uri_put ( root_path );
		root_path = NULL;
	}

	/* If we have both a filename and a root path, ignore an
	 * unsupported URI scheme in the root path, since it may
	 * represent an NFS root.
	 */
	if ( filename && root_path &&
	     ( xfer_uri_opener ( root_path->scheme ) == NULL ) ) {
		printf ( "Ignoring unsupported root path\n" );
		uri_put ( root_path );
		root_path = NULL;
	}

	/* Check that we have something to boot */
	if ( ! ( filename || root_path ) ) {
		rc = -ENOENT_BOOT;
		printf ( "Nothing to boot: %s\n", strerror ( rc ) );
		goto err_no_boot;
	}

	/* Boot using next server, filename and root path */
	if ( ( rc = uriboot ( filename, root_path, san_default_drive(),
			      ( root_path ? 0 : URIBOOT_NO_SAN ) ) ) != 0 )
		goto err_uriboot;

 err_uriboot:
 err_no_boot:
	uri_put ( root_path );
 err_root_path:
	uri_put ( filename );
 err_filename:
 err_pxe_menu_boot:
 err_dhcp:
 err_ifopen:
	return rc;
}

/**
 * Boot the system
 */
int autoboot ( void ) {
	struct net_device *boot_netdev;
	struct net_device *netdev;
	int rc = -ENODEV;

	/* If we have an identifable boot device, try that first */
	if ( ( boot_netdev = find_boot_netdev() ) )
                #ifndef ALU_MOD
		rc = netboot ( boot_netdev );
                #else
		rc = netboot_withRetry ( boot_netdev );
                #endif

	/* If that fails, try booting from any of the other devices */
	for_each_netdev ( netdev ) {
		if ( netdev == boot_netdev )
			continue;
                #ifndef ALU_MOD
		rc = netboot ( netdev );
                #else
		rc = netboot_withRetry ( netdev );
                #endif /* ALU_MOD */
	}

	#ifndef ALU_MOD
	printf ( "No more network devices\n" );
	#else   /* ALU MOD */
	printf ( "No more network devices, reset to re-try\n" );
        /* reset back to BIOS to restart */
        rst_via_ich_to_bios();
	#endif /* ALU_MOD */

	return rc;
}
