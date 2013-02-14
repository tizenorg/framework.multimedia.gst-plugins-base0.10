/**************************************************************************

Copyright 2010 - 2013 Samsung Electronics co., Ltd. All Rights Reserved.

Contact: Boram Park <boram1288.park@samsung.com>

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sub license, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice (including the
next paragraph) shall be included in all copies or substantial portions
of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

**************************************************************************/


#ifndef __XV_TYPES_H__
#define __XV_TYPES_H__

#define XV_PUTIMAGE_HEADER	0xDEADCD01
#define XV_PUTIMAGE_VERSION	0x00010001

/* Return Values */
#define XV_OK 0
#define XV_HEADER_ERROR -1
#define XV_VERSION_MISMATCH -2

/* Video Mode */
#define DISPLAY_MODE_DEFAULT                                      0
#define DISPLAY_MODE_PRI_VIDEO_ON_AND_SEC_VIDEO_FULL_SCREEN       1
#define DISPLAY_MODE_PRI_VIDEO_OFF_AND_SEC_VIDEO_FULL_SCREEN      2

/* Buffer Type */
#define XV_BUF_TYPE_DMABUF  0
#define XV_BUF_TYPE_LEGACY  1

/* Data structure for XvPutImage / XvShmPutImage */
typedef struct {
	unsigned int _header; /* for internal use only */
	unsigned int _version; /* for internal use only */

	unsigned int YBuf;
	unsigned int CbBuf;
	unsigned int CrBuf;
	unsigned int BufType;
} XV_PUTIMAGE_DATA, * XV_PUTIMAGE_DATA_PTR;

static void XV_PUTIMAGE_INIT_DATA(XV_PUTIMAGE_DATA_PTR data)
{
	data->_header = XV_PUTIMAGE_HEADER;
	data->_version = XV_PUTIMAGE_VERSION;
}

#endif /* __XV_TYPES_H__ */
