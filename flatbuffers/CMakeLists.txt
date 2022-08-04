add_custom_command(
    OUTPUT fbs/fbs.stamp
    DEPENDS /root/quantraserver/flatbuffers/fbs/*.fbs
    COMMAND flatc --cpp --gen-object-api -o /root/quantraserver/flatbuffers/cpp  /root/quantraserver/flatbuffers/fbs/*.fbs
    COMMAND flatc --python -o /root/quantraserver/flatbuffers/python  /root/quantraserver/flatbuffers/fbs/*.fbs
    )

add_custom_target(
        fatbuffers ALL
        DEPENDS fbs/fbs.stamp
    )
