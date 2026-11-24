    .section .rodata
    .global zen_image
    .global zen_image_size
    .balign 16
zen_image:
    .incbin GENERATED_IMAGE
zen_image_end:
    .balign 4
zen_image_size:
    .int zen_image_end - zen_image
    .section .note.GNU-stack,"",%progbits
