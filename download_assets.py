#!/usr/bin/env python3
# took from https://github.com/SaschaWillems/Vulkan

import sys
from urllib.request import urlretrieve
from zipfile import ZipFile

# TODO
ASSET_PACK_URL = ''
ASSET_PACK_FILE_NAME = ''

print("Downloading asset pack from '%s'" % ASSET_PACK_URL)    

def reporthook(blocknum, blocksize, totalsize):
    bytesread = blocknum * blocksize
    if totalsize > 0:
        percent = bytesread * 1e2 / totalsize
        s = "\r%5.1f%% (%*d / %d bytes)" % (percent, len(str(totalsize)), bytesread, totalsize)
        sys.stderr.write(s)
        if bytesread >= totalsize:
            sys.stderr.write("\n")
    else:
        sys.stderr.write("read %d\n" % (bytesread,))

urlretrieve(ASSET_PACK_URL, ASSET_PACK_FILE_NAME, reporthook)

print("Download finished")

print("Extracting assets")

zip = ZipFile(ASSET_PACK_FILE_NAME, 'r')
zip.extractall("./assets")
zip.close()

