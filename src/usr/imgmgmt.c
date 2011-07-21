/*    $Alcatel-Lucent: imgmgmt.c,v 1.0.1 2011/06/16 20:58:30 binl Exp $    */


/*
 * Copyright (C) 2007 Michael Brown <mbrown@fensystems.co.uk>.
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

FILE_LICENCE ( GPL2_OR_LATER );

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <ipxe/image.h>
#include <ipxe/downloader.h>
#include <ipxe/monojob.h>
#include <ipxe/open.h>
#include <ipxe/uri.h>
#include <usr/imgmgmt.h>

#define ALU_MOD

#ifdef ALU_MOD
#include <../zlib/puff.h>
#include <elf.h>
#include <ipxe/umalloc.h> /* urealloc */
/* inflate_util.c */
extern int inflate_v(unsigned char * src, unsigned char * dest, int nBytes, unsigned long * nBytesDest);

/* cbBootImage was about 26.6 MB, after compress, it is about 9.xMB
 * so the MAX deflate rate is about 3 times 
 * using inflate_v factor as 10 which will cover the highest compress rate
 */
#define inflate_v_factor_c 10 

/* reset system via ich */
extern void rst_via_ich_to_bios(void);

#define SINGLE_UNCMP
#undef  SINGLE_UNCMP

/* for atca compute blade, only support wind river hypervisor (elf file) for now */
#undef   SUPPORT_ELF_ONLY
#define  SUPPORT_ELF_ONLY


/* check the buffer to be a elf file or not
 * return 0 : if the buffer is not elf file
 * return 1 : yes. Elf file
 * typical elf file header
 * binl@binl-ThinkPad-T410:~/Desktop/netLoader/puff$ xxd -l 100 cbImage 
 * 0000000: 7f45 4c46 0101 0100 0000 0000 0000 0000  .ELF............
 */
unsigned isElfFile(unsigned char *buf, unsigned long buflen) {

  if ( (buflen < 4) ||
       (ELFMAG0 != buf[EI_MAG0]) ||
       (ELFMAG1 != buf[EI_MAG1]) ||
       (ELFMAG2 != buf[EI_MAG2]) ||
       (ELFMAG3 != buf[EI_MAG3]) ) {
     /* not elfp file format */
     return 0;
  }

  return 1;
}

/* check the buffer to be a gz file or not
 * return 0 : if the buffer is not gz file
 * return offset of the deflate data
 */
unsigned isGzFile(unsigned char *buf, unsigned long buflen) {
  const unsigned char gzip_magic_0_c = 0x1f; 
  const unsigned char gzip_magic_1_c = 0x8b; 
  const unsigned char compress_method_c = 0x08; 

  unsigned offset = 0x0a;

  if ( (gzip_magic_0_c != buf[0]) ||
       (gzip_magic_1_c != buf[1]) ||
       (compress_method_c != buf[3]) ) {
     /* not gzip file format */
     return 0;
  }

  for ( offset = 11; offset < buflen; offset++ ) {
    if ( 0x00 == buf[offset] ) {
       offset++;
       break;
    }
  }
  return offset;
}

/* check the buffer to be a deflatable compressed file or not
 * return 0 : if the buffer is not compressed file compressed by deflate
 * return offset of the deflate data
 */
unsigned isDeflatableFile(unsigned char *buf, unsigned long buflen) {
  const unsigned char magic_0_c = 0x78; 
  const unsigned char compress_method_c = 0x08; 

  unsigned offset = 0x0a;

  if ( (buflen <= 3)          ||
       (magic_0_c != buf[1]) ||
       (compress_method_c != buf[0]) ) {
     /* not gzip file format */
     return 0;
  }
  offset = 3;

  return offset;
}

#endif /* ALU_MOD */

/** @file
 *
 * Image management
 *
 */

/**
 * Register an image and leave it registered
 *
 * @v image		Executable image
 * @ret rc		Return status code
 *
 * This function assumes an ownership of the passed image.
 */
int register_and_put_image ( struct image *image ) {
	int rc;

	rc = register_image ( image );
	image_put ( image );
	return rc;
}

/**
 * Register and probe an image
 *
 * @v image		Executable image
 * @ret rc		Return status code
 *
 * This function assumes an ownership of the passed image.
 */
int register_and_probe_image ( struct image *image ) {
	int rc;

	if ( ( rc = register_and_put_image ( image ) ) != 0 )
		return rc;

	if ( ( rc = image_probe ( image ) ) != 0 )
		return rc;

	return 0;
}

/**
 * Register and select an image
 *
 * @v image		Executable image
 * @ret rc		Return status code
 *
 * This function assumes an ownership of the passed image.
 */
int register_and_select_image ( struct image *image ) {
	int rc;

	if ( ( rc = register_and_probe_image ( image ) ) != 0 )
		return rc;

	if ( ( rc = image_select ( image ) ) != 0 )
		return rc;

	return 0;
}

/**
 * Register and boot an image
 *
 * @v image		Image
 * @ret rc		Return status code
 *
 * This function assumes an ownership of the passed image.
 */
int register_and_boot_image ( struct image *image ) {
	int rc;
 
#ifdef ALU_MOD
        unsigned skip = 0;
        unsigned char *source = NULL, *dest;
        size_t len = 0;
        unsigned long sourcelen, destlen;

        /* cbBootImage was about 26.6 MB, after compress, it is about 9.xMB, about 3 times */
        len = image->len;
        source = (unsigned char *)(image->data);
        destlen = inflate_v_factor_c * image->len;
        dest = NULL;

        /* check if deflate file first */
        if ( 0 == skip ) skip = isDeflatableFile(source, len); 
        if ( (0 != skip) && (skip < len) ) {
          dest = (unsigned char *) urealloc((userptr_t)NULL, destlen);
          sourcelen = len;

          rc = inflate_v(source, dest, sourcelen, &destlen);

          if ( 0 != rc) {
            printf("inflate_v() failed (%d) to uncompress %s\n", rc, image->name);
              /* reset back to BIOS to restart */
              rst_via_ich_to_bios();
          } else {
              printf( "inflate_v() succeeded uncompressing %ld bytes\n", destlen);
          }

          image->data = (userptr_t) dest;
          image->len = (size_t) destlen;

        } else { 
          /* check if gz file */
          /* note: the simpler inflate_v version puff method may not be able to uncompress all gz files */

          #ifdef SINGLE_UNCMP
          dest = (unsigned char *) urealloc((userptr_t)NULL, destlen);
          #endif
          skip = isGzFile( (unsigned char *)(image->data), image->len);
          len -= skip;
          if ( 0 != skip ) {
            sourcelen = len;

            printf("skip (%d) to uncompress %s\n", skip, image->name);
            /* first uncompress is just for getting correct file size 
             *   if desk == NULL
             *         printf("desk (0x%lx) dskLen(0x%lx) src(0x%lx) srcLen(0x%lx)\n", (long int)dest, (long int)destlen, (long int)(source + skip), (long int) sourcelen );
             */
            rc = puff(dest, &destlen, source + skip, &sourcelen);
            if (rc) {
              printf("puff() failed (%d) to uncompress %s\n", rc, image->name);
              /* reset back to BIOS to restart */
              rst_via_ich_to_bios();
            } else {
              printf( "puff() succeeded uncompressing %ld bytes\n", destlen);
              if (sourcelen < len) 
                  printf( "%ld compressed bytes unused\n", len - sourcelen);
            }

            #ifndef SINGLE_UNCMP
            dest = (unsigned char *) urealloc((userptr_t)NULL, destlen);
            if ( NULL == dest ) {
              printf("reboot: NOT enough memory for dest buffer Size %lu\n", destlen);
               /* reset back to BIOS to restart */
               rst_via_ich_to_bios();
            } else {
               /* printf("dest buffer %p Size 0x%x\n", dest, (int)destlen); */
            }

            source = (unsigned char *)(image->data);
            sourcelen = len;
            printf("skip (%d) to uncompress %s\n", skip, image->name);
            printf("desk (0x%lx) dskLen(0x%lx) src(0x%lx) srcLen(0x%lx)\n", (long int)dest, (long int)destlen, (long int)(source + skip), (long int) sourcelen );
            rc = puff(dest, &destlen, source + skip, &sourcelen);
            if (rc) {
               printf("puff() failed (%d) to uncompress %s\n", rc, image->name);
               /* reset back to BIOS to restart */
               rst_via_ich_to_bios();
            }
            else {
              printf( "puff() succeeded uncompressing %ld bytes\n", destlen);
              if (sourcelen < len) printf( "%ld compressed bytes unused\n",
                                               len - sourcelen);
            }
            #endif

            image->data = (userptr_t) dest;
            image->len = (size_t) destlen;
          }

        } 

	#ifdef  SUPPORT_ELF_ONLY
        if (  0 == isElfFile( (unsigned char *)(image->data), image->len) ) {
          printf("reboot: NOT ELF file %s\n", image->name);
          /* reset back to BIOS to restart */
          rst_via_ich_to_bios();
        };
	#endif /*  SUPPORT_ELF_ONLY */
#endif /* ALU_MOD */

	if ( ( rc = register_and_select_image ( image ) ) != 0 )
		return rc;

#ifdef ALU_MOD 
	if ( ( rc = image_exec ( image ) ) != 0 )
        {
           /* NOTE : The PXE file type check is only file size check. 
	    * If image is a smaller size corrpted image, 
	    * the corrupted small size image will still be passed to CPU, 
	    * ending with hanging system
	    */
           printf("reboot: not able to exec %s\n", image->name);
           /* reset back to BIOS to restart */
           rst_via_ich_to_bios();
        }
#else /* vanilla code */
	if ( ( rc = image_exec ( image ) ) != 0 )
		return rc;
#endif /* ALU_MOD */

	return 0;
}

/**
 * Register and replace image
 *
 * @v image		Image
 * @ret rc		Return status code
 *
 * This function assumes an ownership of the passed image.
 */
int register_and_replace_image ( struct image *image ) {
	int rc;

	if ( ( rc = register_and_probe_image ( image ) ) != 0 )
		return rc;

	if ( ( rc = image_replace ( image ) ) != 0 )
		return rc;

	return 0;
}

/**
 * Download an image
 *
 * @v uri		URI
 * @v name		Image name, or NULL to use default
 * @v cmdline		Command line, or NULL for no command line
 * @v action		Action to take upon a successful download
 * @ret rc		Return status code
 */
int imgdownload ( struct uri *uri, const char *name, const char *cmdline,
		  int ( * action ) ( struct image *image ) ) {
	struct image *image;
	size_t len = ( unparse_uri ( NULL, 0, uri, URI_ALL ) + 1 );
	char uri_string_redacted[len];
	const char *password;
	int rc;

	/* Allocate image */
	image = alloc_image();
	if ( ! image )
		return -ENOMEM;

	/* Set image name */
	if ( name )
		image_set_name ( image, name );

	/* Set image URI */
	image_set_uri ( image, uri );

	/* Set image command line */
	image_set_cmdline ( image, cmdline );

	/* Redact password portion of URI, if necessary */
	password = uri->password;
	if ( password )
		uri->password = "***";
	unparse_uri ( uri_string_redacted, sizeof ( uri_string_redacted ),
		      uri, URI_ALL );
	uri->password = password;

	/* Create downloader */
	if ( ( rc = create_downloader ( &monojob, image, LOCATION_URI,
					uri ) ) != 0 ) {
		image_put ( image );
		return rc;
	}

	/* Wait for download to complete */
	if ( ( rc = monojob_wait ( uri_string_redacted ) ) != 0 ) {
		image_put ( image );
		return rc;
	}

	/* Act upon downloaded image.  This action assumes our
	 * ownership of the image.
	 */
	if ( ( rc = action ( image ) ) != 0 )
		return rc;

	return 0;
}

/**
 * Download an image
 *
 * @v uri_string	URI as a string (e.g. "http://www.nowhere.com/vmlinuz")
 * @v name		Image name, or NULL to use default
 * @v cmdline		Command line, or NULL for no command line
 * @v action		Action to take upon a successful download
 * @ret rc		Return status code
 */
int imgdownload_string ( const char *uri_string, const char *name,
			 const char *cmdline,
			 int ( * action ) ( struct image *image ) ) {
	struct uri *uri;
	int rc;

	if ( ! ( uri = parse_uri ( uri_string ) ) )
		return -ENOMEM;

	rc = imgdownload ( uri, name, cmdline, action );

	uri_put ( uri );
	return rc;
}

/**
 * Display status of an image
 *
 * @v image		Executable/loadable image
 */
void imgstat ( struct image *image ) {
	printf ( "%s : %zd bytes", image->name, image->len );
	if ( image->type )
		printf ( " [%s]", image->type->name );
	if ( image->flags & IMAGE_SELECTED )
		printf ( " [SELECTED]" );
	if ( image->cmdline )
		printf ( " \"%s\"", image->cmdline );
	printf ( "\n" );
}

/**
 * Free an image
 *
 * @v image		Executable/loadable image
 */
void imgfree ( struct image *image ) {
	unregister_image ( image );
}
