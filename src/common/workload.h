#ifndef WORKLOAD_H_
#define WORKLOAD_H_

#include <vector>
#include <unordered_map>
#include "transaction.h"

using namespace std;


class workload {

public:
	vector<transaction> txns;
};

#endif /* WORKLOAD_H_ */
