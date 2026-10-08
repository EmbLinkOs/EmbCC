set pagination off
set confirm off
set width 0
info registers pc sp
break compute
continue
info registers
bt
info args
info locals
finish
next
next
step
info locals
stepi
nexti
info registers pc
x/8xw &table
print origin
set var origin.x = 10
print origin
delete
watch counter
continue
continue
delete
rwatch table[3]
continue
bt
delete
break gdb-fw.c:@SUM@
continue
print s
print i
next
next
next
print s
x/4i $pc
print/x $sp
print $xpsr
print $msp
print $psp
print $control
print $primask
delete
awatch scale
continue
info all-registers
delete
info args
bt
continue
