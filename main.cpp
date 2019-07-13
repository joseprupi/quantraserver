#include "crow.h"

#include "./request/datesCalendarsRequest.h"
#include "./request/floatingRateBondPricingRequest.h"
#include "./request/fixedRateBondPricingRequest.h"
#include "./request/zeroCouponBondPricingRequest.h"
#include "./request/zeroRateCurveRequest.h"
#include "./request/fwdRateCurveRequest.h"
#include "./request/nodesRequest.h"
#include "./request/discountRateCurveRequest.h"
#include "./request/vanillaInterestRateSwapPricingRequest.h"
#include "./request/scheduleDatesRequest.h"
#include "./request/volatilitySurfaceRequest.h"
#include "./request/optionPricingRrequest.h"
#include "./request/optionMultiPricingRrequest.h"
#include "spdlog/spdlog.h"

namespace spd = spdlog;

class FileLogHandler : public crow::ILogHandler {
    std::shared_ptr<spdlog::logger> my_logger = spd::basic_logger_mt("basic_logger", "quantralog.txt");
    //std::shared_ptr<spdlog::logger> my_logger = spdlog::rotating_logger_mt("file_logger2", "quantralog.txt", 1024 * 1024 * 5, 3);

public:
    void log(std::string message, crow::LogLevel level) override {
        cerr << message;
        my_logger->info(message);
    }
};

int main(int argc, char * argv [])
{
    crow::App<> app;

    CROW_ROUTE(app, "/")
            .name("hello")
                    ([]{
                        return "Hello World!";
                    });
    //<editor-fold desc="Dates and calendars">

    CROW_ROUTE(app, "/isLeap")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        datesCalendarsRequest request;
        request.isLeapYear(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });

    CROW_ROUTE(app, "/endOfMonth")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        datesCalendarsRequest request;
        request.endOfMonth(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });

    CROW_ROUTE(app, "/isEndOfMonth")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        datesCalendarsRequest request;
        request.isEndOfMonth(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });

    CROW_ROUTE(app, "/nextWeekDay")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        datesCalendarsRequest request;
        request.nextWeekDay(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });

    CROW_ROUTE(app, "/nthWeekDay")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        datesCalendarsRequest request;
        request.nthWeekDay(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });

    CROW_ROUTE(app, "/isBusinessDay")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        datesCalendarsRequest request;
        request.isBusinessDay(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });

    CROW_ROUTE(app, "/isHoliday")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        datesCalendarsRequest request;
        request.isHoliday(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });

    CROW_ROUTE(app, "/holidayList")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        datesCalendarsRequest request;
        request.holidayList(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });

    CROW_ROUTE(app, "/adjustDate")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        datesCalendarsRequest request;
        request.adjust(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });

    CROW_ROUTE(app, "/advanceDate")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        datesCalendarsRequest request;
        request.advance(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });

    CROW_ROUTE(app, "/businessDaysBetween")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        datesCalendarsRequest request;
        request.businessDaysBetween(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });

    CROW_ROUTE(app, "/isIMMdate")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        datesCalendarsRequest request;
        request.isIMMdate(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });

    CROW_ROUTE(app, "/nextIMMdate")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        datesCalendarsRequest request;
        request.nextIMMdate(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });

    CROW_ROUTE(app, "/yearFraction")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        datesCalendarsRequest request;
        request.yearFraction(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });
    //</editor-fold>

    //<editor-fold desc="Curve functions">
    CROW_ROUTE(app, "/discountCurve")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        discountRateCurveRequest request;
        request.processRequest(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });

    CROW_ROUTE(app, "/zeroRateCurve")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        zeroRateCurveRequest request;
        request.processRequest(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });

    CROW_ROUTE(app, "/fwdRateCurve")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        fwdRateCurveRequest request;
        request.processRequest(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });

    CROW_ROUTE(app, "/bootstrapNodes")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        nodesRequest request;
        request.processRequest(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });
    //</editor-fold>

    //<editor-fold desc="Enumerations">
    CROW_ROUTE(app, "/getPointTypes")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        crow::json::wvalue response;

        response["response"] = "ok";

        int i = 0;
        for(map<std::string, PointType>::iterator it = PointTypeString.begin(); it != PointTypeString.end(); ++it) {

            response["message"]["PointType"][i] = it->first;
            i++;
        }

        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    CROW_ROUTE(app, "/getCalendars")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        crow::json::wvalue response;

        response["response"] = "ok";

        int i = 0;
        for(map<std::string, CalendarEnum>::iterator it = CalendarString.begin(); it != CalendarString.end(); ++it) {

            response["message"]["Calendar"][i] = it->first;
            i++;
        }

        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    CROW_ROUTE(app, "/getTimeUnits")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        crow::json::wvalue response;

        response["response"] = "ok";

        int i = 0;
        for(map<std::string, TimeUnitEnum>::iterator it = TimeUnitString.begin(); it != TimeUnitString.end(); ++it) {

            response["message"]["TimeUnit"][i] = it->first;
            i++;
        }

        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    CROW_ROUTE(app, "/getBusinessDayConventions")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        crow::json::wvalue response;

        response["response"] = "ok";

        int i = 0;
        for(map<std::string, BusinessDayConventionEnum>::iterator it = BusinessDayConventionString.begin(); it != BusinessDayConventionString.end(); ++it) {

            response["message"]["BusinessDayConvention"][i] = it->first;
            i++;
        }

        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    CROW_ROUTE(app, "/getDayCounters")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        crow::json::wvalue response;

        response["response"] = "ok";

        int i = 0;
        for(map<std::string, DayCounterEnum>::iterator it = DayCounterString.begin(); it != DayCounterString.end(); ++it) {

            response["message"]["DayCounter"][i] = it->first;
            i++;
        }

        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    CROW_ROUTE(app, "/getFrequencys")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        crow::json::wvalue response;

        response["response"] = "ok";

        int i = 0;
        for(map<std::string, FrequencyEnum>::iterator it = FrequencyString.begin(); it != FrequencyString.end(); ++it) {

            response["message"]["Frequency"][i] = it->first;
            i++;
        }

        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    CROW_ROUTE(app, "/getIbors")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        crow::json::wvalue response;

        response["response"] = "ok";

        int i = 0;
        for(map<std::string, IborEnum>::iterator it = IborString.begin(); it != IborString.end(); ++it) {

            response["message"]["Ibor"][i] = it->first;
            i++;
        }

        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    CROW_ROUTE(app, "/getOvernightIndex")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        crow::json::wvalue response;

        response["response"] = "ok";

        int i = 0;
        for(map<std::string, OvernightIndexEnum>::iterator it = OvernightIndexString.begin(); it != OvernightIndexString.end(); ++it) {

            response["message"]["OvernightIndex"][i] = it->first;
            i++;
        }

        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    CROW_ROUTE(app, "/getCompoundings")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        crow::json::wvalue response;

        response["response"] = "ok";

        int i = 0;
        for(map<std::string, CompoundingEnum>::iterator it = CompoundingString.begin(); it != CompoundingString.end(); ++it) {

            response["message"]["Compounding"][i] = it->first;
            i++;
        }

        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    CROW_ROUTE(app, "/getInterpolators")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        crow::json::wvalue response;

        response["response"] = "ok";

        int i = 0;
        for(map<std::string, InterpolationEnum>::iterator it = InterpolationString.begin(); it != InterpolationString.end(); ++it) {

            response["message"]["Interpolator"][i] = it->first;
            i++;
        }

        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    CROW_ROUTE(app, "/getSwapTypes")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        crow::json::wvalue response;

        response["response"] = "ok";

        int i = 0;
        for(map<std::string, SwapTypeEnum>::iterator it = SwapTypeString.begin(); it != SwapTypeString.end(); ++it) {

            response["message"]["SwapType"][i] = it->first;
            i++;
        }

        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    CROW_ROUTE(app, "/getDateGenerationRules")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        crow::json::wvalue response;

        response["response"] = "ok";

        int i = 0;
        for(map<std::string, DateGenerationEnum>::iterator it = DateGenerationString.begin(); it != DateGenerationString.end(); ++it) {

            response["message"]["DateGenerationRule"][i] = it->first;
            i++;
        }

        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    CROW_ROUTE(app, "/getWeekDays")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        crow::json::wvalue response;

        response["response"] = "ok";

        int i = 0;
        for(map<std::string, WeekDayEnum>::iterator it = WeekDayString.begin(); it != WeekDayString.end(); ++it) {

            response["message"]["WeekDay"][i] = it->first;
            i++;
        }

        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    CROW_ROUTE(app, "/getMonths")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        crow::json::wvalue response;

        response["response"] = "ok";

        int i = 0;
        for(map<std::string, MonthEnum>::iterator it = MonthString.begin(); it != MonthString.end(); ++it) {

            response["message"]["Month"][i] = it->first;
            i++;
        }

        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });

    CROW_ROUTE(app, "/getBootstrapTraits")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        crow::json::wvalue response;

        response["response"] = "ok";

        int i = 0;
        for(map<std::string, BootstrapTrait>::iterator it = BootstrapTraitString.begin(); it != BootstrapTraitString.end(); ++it) {

            response["message"]["BootstrapTrait"][i] = it->first;
            i++;
        }

        crow::response res(response);
        res.add_header("Access-Control-Allow-Origin", "*");
        return res;
    });
    //</editor-fold>

    //<editor-fold desc="Pricing">
    CROW_ROUTE(app, "/priceVanillaSwap")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        vanillaInterestRateSwapPricingRequest request;
        request.processRequest(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });

    CROW_ROUTE(app, "/priceZeroCouponBond")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;
        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        zeroCouponBondPricingRequest request;
        request.processRequest(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });

    CROW_ROUTE(app, "/priceFixedRateBond")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        fixedRateBondPricingRequest request;
        request.processRequest(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });

    CROW_ROUTE(app, "/priceFloatingRateBond")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        floatingRateBondPricingRequest request;
        request.processRequest(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });
    //</editor-fold>

    //<editor-fold desc="Schedule">
    CROW_ROUTE(app, "/scheduleDates")
            .methods("POST"_method)
    ([](const crow::request& req){

        CROW_LOG_DEBUG  << req.body;

        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

        scheduleDatesRequest request;
        request.processRequest(req, response);

        crow::response res((*response));
        res.add_header("Access-Control-Allow-Origin", "*");

        return res;

    });
    //</editor-fold>

    //<editor-fold desc="Schedule">
    CROW_ROUTE(app, "/volatilitySurface")
            .methods("POST"_method)
                    ([](const crow::request& req){

                        CROW_LOG_DEBUG  << req.body;

                        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

                        volatilitySurfaceRequest request;
                        request.processRequest(req, response);

                        crow::response res((*response));
                        res.add_header("Access-Control-Allow-Origin", "*");

                        return res;

                    });
    //</editor-fold>

    //<editor-fold desc="Schedule">
    CROW_ROUTE(app, "/optionPricing")
            .methods("POST"_method)
                    ([](const crow::request& req){

                        CROW_LOG_DEBUG  << req.body;

                        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

                        optionPricingRequest request;
                        request.processRequest(req, response);

                        crow::response res((*response));
                        res.add_header("Access-Control-Allow-Origin", "*");

                        return res;

                    });
    //</editor-fold>

    //<editor-fold desc="Schedule">
    CROW_ROUTE(app, "/multiOptionPricing")
            .methods("POST"_method)
                    ([](const crow::request& req){

                        CROW_LOG_DEBUG  << req.body;

                        boost::shared_ptr<crow::json::wvalue> response(new crow::json::wvalue());

                        optionMultiPricingRrequest request;
                        request.processRequest(req, response);

                        crow::response res((*response));
                        res.add_header("Access-Control-Allow-Origin", "*");

                        return res;

                    });
    //</editor-fold>

    crow::logger::setLogLevel(crow::LogLevel::DEBUG);
    boost::shared_ptr<FileLogHandler> x = boost::make_shared<FileLogHandler>();
    crow::logger::setHandler(x.get());

    if(argc >1){
        app.port(atoi(argv[1]))
                .run();
    }else {
        app.port(18080)
                .run();
    }

}