#!/bin/bash

PROGNAME=`basename "$0"`

# Script to extract a function from a project.

# Function to extract
FUNCTION=

# Project to extract from
PACKAGE_PATH=

# Path to CCP-POL scripts
P="$HOME/projects/kgr/scripts/ccp-pol"

# Path to compiled KLP-CCP
CCP="$HOME/projects/kgr/ccp/build/klp-ccp --pol-cmd-may-include-header=$P/kgr-ccp-pol-may-include-header.sh --pol-cmd-can-externalize-fun=$P/kgr-ccp-pol-can-externalize-fun.sh --pol-cmd-shall-externalize-fun=$P/kgr-ccp-pol-shall-externalize-fun.sh --pol-cmd-shall-externalize-obj=$P/kgr-ccp-pol-shall-externalize-obj.sh --pol-cmd-modify-externalized-sym=$P/kgr-ccp-pol-modify-externalized-sym.sh --pol-cmd-modify-patched-fun-sym=$P/kgr-ccp-pol-modify-patched-sym.sh --pol-cmd-rename-rewritten-fun=$P/kgr-ccp-pol-rename-rewritten-fun.sh"

CCP_CFLAGS="-I/usr/include/
           -I/usr/include/bits/types/
           -I/usr/lib64/gcc/x86_64-suse-linux/8/include/
           -I/usr/lib64/gcc/x86_64-suse-linux/8/include-fixed
           -Icrypto/include
           -Iinclude
           -I/usr/local/include
           -I/home/giulianob/klp-openssl/pos-patch/openssl-1.1.1d/include
           -I.
           -fPIC
           -pthread
           -m64
           -Wa,--noexecstack
           -Wall
           -O3
           -O2
           -Wall
           -fstack-protector-strong
           -funwind-tables
           -fasynchronous-unwind-tables
           -fstack-clash-protection
           -Werror=return-type
           -flto=auto
           -g
           -fpatchable-function-entry=16,14
           -fdump-ipa-clones
           -g3
           -Wa,--noexecstack
           -fno-common
           -Wall
           -DOPENSSL_USE_NODELETE
           -DL_ENDIAN
           -DOPENSSL_PIC
           -DOPENSSL_CPUID_OBJ
           -DOPENSSL_IA32_SSE2
           -DOPENSSL_BN_ASM_MONT
           -DOPENSSL_BN_ASM_MONT5
           -DOPENSSL_BN_ASM_GF2m
           -DSHA1_ASM
           -DSHA256_ASM
           -DSHA512_ASM
           -DKECCAK1600_ASM
           -DRC4_ASM
           -DMD5_ASM
           -DVPAES_ASM
           -DGHASH_ASM
           -DECP_NISTZ256_ASM
           -DX25519_ASM
           -DPOLY1305_ASM
           -DOPENSSLDIR="\"/etc/ssl\""
           -DENGINESDIR="\"/usr/lib64/engines-1.1\""
           -DNDEBUG
           -D_FORTIFY_SOURCE=2
           -DTERMIO
           -DPURIFY
           -D_GNU_SOURCE
           -DOPENSSL_NO_BUF_FREELISTS
           -D__THROW
           -MMD
           -MF ssl/s3_lib.d.tmp
           -MT ssl/s3_lib.c
           -c
           -o ssl/s3_lib.o
           ssl/s3_lib.c"

# Variables used in klp-ccp
KCP_READELF=readelf
KCP_RENAME_PREFIX=klp
KCP_WORK_DIR=$HOME/klp-openssl/ccp/c/work
KCP_KBUILD_SDIR=/home/giulianob/klp-openssl/pos-patch/openssl-1.1.1d
KCP_KBUILD_ODIR=/home/giulianob/klp-openssl/pos-patch/openssl-1.1.1d
KCP_MOD_SYMVERS=/home/giulianob/klp-openssl/Modules.symver
KCP_PATCHED_OBJ=/home/giulianob/klp-openssl/pos-patch/bin/libssl.so.1.1
KCP_IPA_CLONES_DUMP=/home/giulianob/klp-openssl/pos-patch/openssl-1.1.1d/ssl/ssl_lib.c.000i.ipa-clones

do_extract_function()
{
  local target_function="ssl3_read"
  local compiler_version="x86_64-gcc-9.1.0"
  local output="/tmp/lp.c"
  pushd /home/giulianob/klp-openssl/pos-patch/openssl-1.1.1d
  set -o xtrace
  $CCP --compiler=$compiler_version \
       -i $target_function \
       -o "$output" \
       -- $CCP_CFLAGS
  set +o xtrace
  popd
}

print_help_message()
{
  echo "Extract function from project and its dependencies."
  echo "Author: Giuliano Belinassi (gbelinassi@suse.de)"
  echo "Based on klp-ccp"
  echo ""
  echo "Usage: $PROGNAME <switches>"
  echo "where <switches>"
  echo "  --function FUNCTION            Function to extract."
  echo "  --package  PATH_TO_PROJECT     Path to project"
  echo ""
}

parse_program_argv()
{
  # If user didn't provide any arugment, then bails out with a help message.
  if [[ -z "$@" ]]; then
    print_help_message
    exit 0
  fi

  # Parse arguments provided by user.
  for i in "$@"; do
    case $i in
      --package=*)
        PACKAGE_PATH="${i#*=}"
        shift
        ;;
      --function=*)
        FUNCTION="${i#*=}"
        shift
        ;;
      --help)
        print_help_message
        exit 0
        shift
        ;;
      -*|--*)
        echo "Unknown option $i"
        echo ""
        print_help_message

        exit 1
        ;;
      *)
        ;;
    esac
  done

}

main()
{
  #parse_program_argv $*

  do_extract_function
}

main $*
