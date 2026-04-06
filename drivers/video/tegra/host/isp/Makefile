GCOV_PROFILE := y
ccflags-y += -Idrivers/video/tegra/host
ccflags-y += -Idrivers/video/tegra/camera

nvhost-isp-objs  = \
		isp.o

obj-$(CONFIG_TEGRA_GRHOST) += nvhost-isp.o
obj-$(CONFIG_TEGRA_GRHOST) += isp_trace.o
obj-$(CONFIG_TEGRA_GRHOST) += isp_patch.o
