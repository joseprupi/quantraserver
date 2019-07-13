//
// Created by Josep Rubio on 8/28/17.
//

#include "termStructureListParser.h"

termStructureListParser::termStructureListParser(std::string message, std::vector<std::string> *errorLog)
        :QuantraParser(message, errorLog){

    this->message = message;

}

void termStructureListParser::setAsOfDate(Date date){
    this->asOfDate = date;
}

std::map<string,boost::shared_ptr<termStructureParser>> termStructureListParser::getTermStructuresList(){
    return termStructuresList;
}

bool termStructureListParser::parse(Value &JSON){

    std::ostringstream erroros;
    bool error = false;

    Value::MemberIterator iterator;
    iterator = JSON.FindMember("Curves");
    if (iterator != JSON.MemberEnd()) {

        if (!JSON["Curves"].IsArray() || JSON["Curves"].GetArray().Size() == 0){
            CROW_LOG_DEBUG << "Curves has to be an array with at least 1 element";
            erroros << "Curves has to be an array with at least 1 element";
            this->getErrorLog()->push_back(erroros.str());
            error = true;
        }else{
            int i = 0;

            while (i < JSON["Curves"].Size()) {
                try{
                    string logstring = "Curves Curve: " + std::to_string(i) + " ";
                    boost::shared_ptr<termStructureParser> tmpStructureParser(new termStructureParser(logstring, this->getErrorLog(), termStructuresList));
                    error = tmpStructureParser->parse(JSON["Curves"][i]);

                    if(!error){

                        tmpStructureParser->setAsOfDate(this->asOfDate);
                        boost::shared_ptr<YieldTermStructure> tmpTermStructure = tmpStructureParser->createTermStructure();

                        if(tmpTermStructure){

                            std::pair<std::map<string,boost::shared_ptr<termStructureParser>>::iterator,bool> res =
                                    termStructuresList.insert(std::make_pair(tmpStructureParser->getId(),tmpStructureParser));

                            if (res.second==false) {
                                CROW_LOG_DEBUG << "Existing Term structure with id: " << tmpStructureParser->getId();
                                erroros << "Existing Term structure with id: " << tmpStructureParser->getId();
                                this->getErrorLog()->push_back(erroros.str());
                                error = true;
                            }
                        }
                    }
                }catch (Error e) {
                    erroros << e.what();
                    CROW_LOG_DEBUG << e.what();
                    this->getErrorLog()->push_back(erroros.str());
                    erroros.str("");
                    erroros.clear();
                    error = true;
                }

                i++;

            }
        }

    } else {
        erroros << "Curves: Not found ";
        CROW_LOG_DEBUG << "Curves: Not found ";
        this->getErrorLog()->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    return error;

}