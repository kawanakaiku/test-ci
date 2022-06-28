# stop execution 10 minutes before reaching 6 hours

init=$(cd /tmp ; cat timer ; rm -f timer)

start=$(date +%s)

end=$(perl -e "print $start + 6*60*60")

remaining_time=$(perl -e "print $end - $start")

duration=$(perl -e "print $remaining_time - 10*60")

if [ $duration -gt 0 ]; then
    timeout --preserve-status --signal=SIGINT $duration "$@"
fi

exit 0
