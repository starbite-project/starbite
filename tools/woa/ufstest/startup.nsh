@echo -off
# Project Starbite W2 UFS read test (tools/woa/ufs-test.sh). Runs from the test
# ESP on CACHE, logs to results.txt there, then reboots.
set OUT none
if exist fs0:\starbite.mrk then
  set OUT fs0:
endif
if exist fs1:\starbite.mrk then
  set OUT fs1:
endif
if exist fs2:\starbite.mrk then
  set OUT fs2:
endif
if exist fs3:\starbite.mrk then
  set OUT fs3:
endif
%OUT%
echo starbite ufs test >a results.txt
ver >>a results.txt
map -r >>a results.txt
devtree >>a results.txt
echo ==== low: GPT header and entries >>a results.txt
for %a run (0 15)
  echo ---- blk%a LBA 0/2 >>a results.txt
  dblk blk%a 0 1 >>a results.txt
  dblk blk%a 2 1 >>a results.txt
endfor
echo ==== mid: USERDATA iOS root, kiosk root >>a results.txt
for %a run (0 15)
  echo ---- blk%a LBA 19EA80/5DEA80 >>a results.txt
  dblk blk%a 19EA80 1 >>a results.txt
  dblk blk%a 5DEA80 1 >>a results.txt
endfor
echo ==== high: last LBA (backup GPT) >>a results.txt
for %a run (0 15)
  echo ---- blk%a LBA EE47FF >>a results.txt
  dblk blk%a EE47FF 1 >>a results.txt
endfor
echo done >>a results.txt
reset
