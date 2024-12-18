# this script generates the linker script 'linkglobals.ld'

BB_TAG_SHIFT = 41

fl = open("linkglobals.ld", "w")
fl.write("ENTRY(_my_start);\n\n")
fl.write("SECTIONS\n")
fl.write("{\n")
for i in range(4, BB_TAG_SHIFT):
    start_mut = i << BB_TAG_SHIFT
    sizeclass = 2 ** i

    section_mut = "basebounds_section_mut_" + str(sizeclass)
    section_mut_end = section_mut + "_end"
    section_const = "basebounds_section_const_" + str(sizeclass)
    section_const_end = section_const + "_end"

    # ensure page alignment between the const and mutable chunks
    # because they need different page permissions
    # also ensure the next chunk starts at the sizeclass alignment for bases
    align_inter = max(0x1000, sizeclass)

    fl.write("\t%s %s : AT(%s)\n" % (section_mut, hex(start_mut), hex(start_mut)))
    fl.write("\t{\n")
    fl.write("\t\tKEEP(*(%s))\n" %(section_mut))
    fl.write("\t\t%s = ALIGN((.), %d);\n" %(section_mut_end, align_inter))
    fl.write("\t}\n")

    fl.write("\t%s %s : AT(%s)\n" % (section_const, section_mut_end, section_mut_end))
    fl.write("\t{\n")
    fl.write("\t\tKEEP(*(%s))\n" %(section_const))
    fl.write("\t\t%s = (.);\n" %(section_const_end))
    print("extern void* " + section_const + "_end;")
    fl.write("\t}\n")

fl.write("}\n")
# XXX: is '.strtab' always present (clang)?
fl.write("INSERT AFTER .strtab;\n")
