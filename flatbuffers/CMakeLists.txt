add_custom_command(
    OUTPUT fbs/fbs.stamp
    DEPENDS /root/quantragrpc/flatbuffers/fbs/*.fbs
    COMMAND flatc --cpp --gen-object-api -o /root/quantragrpc/flatbuffers/cpp  /root/quantragrpc/flatbuffers/fbs/*.fbs
    COMMAND flatc --python -o /root/quantragrpc/flatbuffers/python  /root/quantragrpc/flatbuffers/fbs/*.fbs
    )

add_custom_target(
        fatbuffers ALL
        DEPENDS fbs/fbs.stamp
    )
