RESOURCES_LIBRARY() 
 
OWNER(heretic) 
 
DECLARE_EXTERNAL_RESOURCE(YOCTO_SDK_ROOT sbr:882588946) 
CFLAGS( 
    GLOBAL -cxx-isystem GLOBAL $YOCTO_SDK_ROOT_RESOURCE_GLOBAL/usr/include/c++/5.3.0/arm-poky-linux-gnueabi 
    GLOBAL -cxx-isystem GLOBAL $YOCTO_SDK_ROOT_RESOURCE_GLOBAL/usr/include/c++/5.3.0 
) 
 
END() 
