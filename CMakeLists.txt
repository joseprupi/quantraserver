cmake_minimum_required(VERSION 3.3)
project(quantraserver)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
SET (BOOST_ROOT "/usr/local/Cellar/boost/1.63.0")

INCLUDE_DIRECTORIES(/home/joseprupi/josep/crow/include ${CMAKE_CURRENT_SOURCE_DIR}/rapidjson ${CMAKE_CURRENT_SOURCE_DIR})

link_directories(/opt/local/lib/)
find_library(QL QuantLib)

find_package(Boost COMPONENTS system filesystem REQUIRED)

if(Boost_FOUND)
    include_directories(${Boost_INCLUDE_DIRS})
    INCLUDE_DIRECTORIES(/opt/local/include/ /Users/joseprubio/Documents/dev/QuantLib-1.10)
    add_executable(quantraserver main.cpp
            common/enums.h
            common/enums.cpp
            parser/indexParser.cpp
            parser/indexParser.h
            parser/floatingRateBondParser.cpp
            parser/floatingRateBondParser.h
            parser/scheduleParser.cpp
            parser/scheduleParser.h
            request/floatingRateBondPricingRequest.cpp
            request/floatingRateBondPricingRequest.h
            parser/termStructureParser.cpp
            parser/termStructureParser.h
            parser/QuantraParser.cpp
            parser/QuantraParser.h
            parser/termStructurePointParser.cpp
            parser/termStructurePointParser.h
            parser/yieldParser.cpp
            parser/yieldParser.h
            parser/pricingParser.cpp
            parser/pricingParser.h
            request/zeroRateCurveRequest.cpp
            request/zeroRateCurveRequest.h
            request/fwdRateCurveRequest.cpp
            request/fwdRateCurveRequest.h
            parser/fwdCurveParser.cpp
            parser/fwdCurveParser.h
            common/common.h
            request/discountRateCurveRequest.cpp
            request/discountRateCurveRequest.h
            request/zeroCouponBondPricingRequest.cpp
            request/zeroCouponBondPricingRequest.h
            parser/zeroCouponBondParser.cpp
            parser/zeroCouponBondParser.h
            request/fixedRateBondPricingRequest.cpp
            request/fixedRateBondPricingRequest.h
            parser/fixedRateBondParser.cpp
            parser/fixedRateBondParser.h
            parser/vanillaInterestRateSwapParser.cpp
            parser/vanillaInterestRateSwapParser.h
            parser/fixedLegParser.cpp
            parser/fixedLegParser.h
            parser/floatingLegParser.cpp
            parser/floatingLegParser.h
            request/vanillaInterestRateSwapPricingRequest.cpp
            request/vanillaInterestRateSwapPricingRequest.h
            request/datesCalendarsRequest.cpp
            request/datesCalendarsRequest.h
            request/nodesRequest.cpp
            request/nodesRequest.h
            parser/termStructureListParser.cpp
            parser/termStructureListParser.h
            parser/zeroCurveParser.cpp
            parser/zeroCurveParser.h
            request/scheduleDatesRequest.cpp
            request/scheduleDatesRequest.h
            parser/zeroCurveSpreadParser.cpp
            parser/zeroCurveSpreadParser.h
            parser/volTermStructureParser.cpp
            parser/volTermStructureParser.h
            parser/volatilitySurfaceParser.h
            parser/volatilitySurfaceParser.cpp
            request/volatilitySurfaceRequest.h
            request/volatilitySurfaceRrequest.cpp
            request/optionPricingRrequest.h
            request/optionPricingRrequest.cpp
            parser/europeanOptionParser.cpp
            parser/europeanOptionParser.h
            parser/optionParser.cpp
            parser/optionParser.h
            parser/europeanOptionParser.cpp
            parser/europeanOptionParser.h
            parser/europeanMultiOptionParser.cpp
            parser/europeanMultiOptionParser.h
            request/optionMultiPricingRrequest.h
            request/optionMultiPricingRrequest.cpp)
    target_link_libraries(quantraserver ${Boost_LIBRARIES})
    TARGET_LINK_LIBRARIES(quantraserver ${QL} -lpthread -lm)
endif()

