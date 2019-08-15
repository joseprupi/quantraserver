FROM fedora:31 AS builder

RUN yum install -qy boost-devel cmake make git QuantLib-devel g++

WORKDIR /opt/

RUN git clone https://github.com/thulio/crow.git && \
    git clone https://github.com/thulio/quantraserver.git && \
    ln -sf /opt/crow/include /usr/local/include/crow

WORKDIR /opt/quantraserver

RUN cmake . && \
    make -j 2

FROM fedora:31

ARG PUID=1000
ARG PGID=1000
ARG HOME="/quantraserver"

EXPOSE 18080

RUN yum install -qy boost QuantLib

RUN groupadd -g ${PGID} quantraserver
RUN useradd -d ${HOME} -g quantraserver -u ${PUID} quantraserver

USER quantraserver

WORKDIR /quantraserver

COPY --from=builder /opt/quantraserver/quantraserver .

CMD ["./quantraserver"]
