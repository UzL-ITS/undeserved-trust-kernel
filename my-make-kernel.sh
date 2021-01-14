#!/bin/bash

run_cmd()
{
   echo "$*"

   eval "$*" || {
      echo "ERROR: $*"
      exit 1
   }
}


[ -d linux-patches ] && {

	for P in linux-patches/*.patch; do
		run_cmd patch -p1 -d linux < $P
	done
}

MAKE="make -j $(getconf _NPROCESSORS_ONLN) LOCALVERSION="

run_cmd $MAKE distclean

pushd linux >/dev/null
	run_cmd cp /boot/config-$(uname -r) .config
	run_cmd ./scripts/config --set-str LOCALVERSION "-sev-es"
	run_cmd ./scripts/config --disable LOCALVERSION_AUTO
	run_cmd ./scripts/config --enable  AMD_MEM_ENCRYPT
	run_cmd ./scripts/config --enable  AMD_MEM_ENCRYPT_ACTIVE_BY_DEFAULT
	run_cmd ./scripts/config --enable  CONFIG_X86_CPUID
	run_cmd ./scripts/config --disable CONFIG_HW_RANDOM_VIRTIO
	run_cmd ./scripts/config --disable CONFIG_CRYPTO_DEV_VIRTIO
	run_cmd ./scripts/config --enable CONFIG_MODULES
popd >/dev/null

run_cmd $MAKE olddefconfig

# Build
run_cmd $MAKE >/dev/null

run_cmd $MAKE bindeb-pkg

