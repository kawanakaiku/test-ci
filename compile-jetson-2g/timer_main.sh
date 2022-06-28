# stop execution 10 minutes before reaching 6 hours

init=$(cd /tmp ; cat timer ; rm -f timer)

end=$(perl -e "print $init + 6*60*60")

start=$(date +%s)

remaining_time=$(perl -e "print $end - $start")

duration=$(perl -e "print $remaining_time - 10*60")

duration=600

if [ $duration -gt 0 ]; then
    echo "killing in ${duration}"
    timeout --verbose --preserve-status --signal=SIGTERM --kill-after=10 ${duration}s "$@"
fi

exit 0
