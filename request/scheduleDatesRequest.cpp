//
// Created by Josep Rubio on 9/26/17.
//

#include "scheduleDatesRequest.h"

void scheduleDatesRequest::processRequest(crow::request req, boost::shared_ptr<crow::json::wvalue> response) {

    auto x = crow::json::load(req.body);

    bool error = false;
    bool tmpBool = false;

    std::ostringstream erroros;

    (*response)["response"] = "ok";

    std::vector<std::string> *errorLog = new std::vector<std::string>();

    /*Temporary to solve "{\"\"}" issue*/
    if(req.body.at(0) == '"'){
        (*response)["response"] = "ko";
        (*response)["message"]["DiscountRateCurveErrors"] = "Wrong format";
        return;
    }

    Document d;
    ParseResult ok = d.Parse(req.body.c_str());

    if (!ok) {
        fprintf(stderr, "JSON parse error: %s (%u)",
                GetParseError_En(ok.Code()), ok.Offset());

        erroros << "JSON parse error: " << GetParseError_En(ok.Code()) << " " << ok.Offset();

        (*response)["response"] = "ko";
        (*response)["message"]["DiscountRateCurveErrors"] = erroros.str();

        return;
    }

    Value::MemberIterator iterator;
    iterator = d.FindMember("Schedule");
    scheduleParser ScheduleParser(errorLog, "Schedule", false);

    if(iterator != d.MemberEnd()){
        Value& scheduleJSON = d["Schedule"];

        error = error || ScheduleParser.parse(scheduleJSON);
    }else{
        erroros << "Schedule: Not found ";
        CROW_LOG_DEBUG << "Schedule: Not found ";
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    std::vector<Date> resultPoints;

    if(!error){

            try{

                boost::shared_ptr<YieldTermStructure> term;

                boost::shared_ptr<Schedule> schedule = ScheduleParser.createSchedule();

                Date counter = schedule->startDate();

                    int i = 0;
                    while (i < schedule->size()) {

                        resultPoints.push_back(schedule->date(i));

                        counter++;
                        i++;
                    }

            }catch (Error e) {
                erroros << e.what();
                CROW_LOG_DEBUG << e.what();
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }

    }

    if(!error){

        (*response)["response"] = "ok";

        for (int i = 0; i < resultPoints.size(); i++) {

            std::ostringstream os;
            os << QuantLib::io::iso_date(resultPoints[i]);
            (*response)["message"]["Dates"][i] = os.str();

        }
    }else{
        (*response)["response"] = "ko";

        int i = 0;
        for (std::vector<string>::iterator it = errorLog->begin(); it != errorLog->end(); ++it) {

            (*response)["message"]["ScheduleErrors"][i] = *it;
            i++;

        }
    }
}