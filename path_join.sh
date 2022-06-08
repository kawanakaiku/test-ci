cat >/dev/null <<'EOF'
path_join() {
  local result='' path
  for path in "`pwd`" "$@" ; do
    [[ ${path} == /* ]] && result="${path}" || result+="/${path}/"
    result="`printf \"${result}\" | perl -pe 's@/+@/@g ; s@/\./@/@g ; s@/([^/]+?|[^(\.\.)])/\.\./@/@g'`"
  done
  [[ ${result:1} == */ ]] && result="${result:0:-1}"
  printf "${result}"
}
EOF

path_join() {
  ( for path in "$@" ; do echo "${path}" ; done ) | awk -F/ '
    BEGIN{
      result = ""
    }
    {
      if ($0 ~ /^\//) {
        # if line startswith /
        # absolute path
        result = $0
      } else if ( result == "" ) {
        result = $0 "/"
      } else {
        # relative path
        result =  result "/" $0 "/"
      }
    }
    END{
      if ( result == "" || result == "/" ) {
        # unchanged or root
        print result
        exit
      }

      # change "//" to "/"
      gsub(/\/+/, "/", result)

      # change "/./" to "/"
      gsub(/\/\.\//, "/", result)

      # process parent dirs
      split(result, array, "/")
      for (i in array) {
        if (array[i] == "..") {
          for (j=i-1; j>=1; j--) {
            if ( j in array ) {
              if ( array[j] == "" ) {
                if ( j != 1 ) { delete array[j] }
                break
              }
              if ( array[j] != ".." ) {
                delete array[j]
                delete array[i]
                break
              }
            }
          }
        }
      }
      result = ""
      for (i in array) {
        result = result array[i] "/"
      }

      # remove / in last
      gsub(/\/+$/, "", result)
      
      print result
    }'
}
