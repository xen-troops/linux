xen-arm-y	+= $(addprefix ../../arm/xen/, enlighten.o grant-table.o p2m.o mm.o)
obj-y		:= xen-arm.o hypercall.o
obj-$(CONFIG_XEN_EFI) += $(addprefix ../../arm/xen/, efi.o)
obj-$(CONFIG_XEN)     += $(addprefix ../../arm/xen/, xt_cma_helper.o)
