$(call inherit-product, device/motorola/obake/full_obake.mk)

# Inherit some common CM stuff.
$(call inherit-product, vendor/XPe/config/common_full_phone.mk)

# Enhanced NFC
$(call inherit-product, vendor/XPe/config/nfc_enhanced.mk)

PRODUCT_NAME := xpe_obake
PRODUCT_RELEASE_NAME := DROID Mini
PRODUCT_DEVICE := obake
PRODUCT_BRAND := Motorola
PRODUCT_MANUFACTURER := Motorola
PRODUCT_MODEL := DROID Mini
