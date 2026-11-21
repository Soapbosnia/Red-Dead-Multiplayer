# Print the C strings at addresses. Usage: -postScript ReadStr.py <out> <addr...>
# @category RDRMP
args = list(getScriptArgs())
out = open(args[0], "w")
for a in args[1:]:
    ad = currentProgram.getAddressFactory().getAddress(a)
    s = ""
    for i in range(64):
        b = getByte(ad.add(i)) & 0xff
        if b == 0:
            break
        s += chr(b)
    out.write("%s: %s\n" % (a, s))
out.close()
