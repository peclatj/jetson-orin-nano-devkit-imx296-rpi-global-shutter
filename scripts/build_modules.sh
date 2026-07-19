cd ~/nvidia/nvidia_sdk/JetPack_6.2.2_Linux_JETSON_ORIN_NANO_TARGETS/Linux_for_Tegra/source
export KERNEL_HEADERS=$PWD/kernel/kernel-jammy-src
make KCFLAGS="-Wno-address -Wno-enum-int-mismatch" modules
