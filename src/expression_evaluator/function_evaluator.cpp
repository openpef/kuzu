#include "include/function_evaluator.h"

#include "src/binder/expression/include/function_expression.h"

namespace graphflow {
namespace evaluator {

void FunctionExpressionEvaluator::init(const ResultSet& resultSet, MemoryManager* memoryManager) {
    BaseExpressionEvaluator::init(resultSet, memoryManager);
    execFunc = ((ScalarFunctionExpression&)*expression).execFunc;
    if (expression->dataType.typeID == BOOL) {
        selectFunc = ((ScalarFunctionExpression&)*expression).selectFunc;
    }
    resultVector = make_shared<ValueVector>(memoryManager, expression->dataType);
    if (children.empty()) {
        resultVector->state = DataChunkState::getSingleValueDataChunkState();
    }
    for (auto& child : children) {
        parameters.push_back(child->resultVector);
    }
}

void FunctionExpressionEvaluator::evaluate() {
    for (auto& child : children) {
        child->evaluate();
    }
    execFunc(parameters, *resultVector);
}

bool FunctionExpressionEvaluator::select(SelectionVector& selVector) {
    for (auto& child : children) {
        child->evaluate();
    }
    return selectFunc(parameters, selVector);
}

unique_ptr<BaseExpressionEvaluator> FunctionExpressionEvaluator::clone() {
    vector<unique_ptr<BaseExpressionEvaluator>> clonedChildren;
    for (auto& child : children) {
        clonedChildren.push_back(child->clone());
    }
    return make_unique<FunctionExpressionEvaluator>(expression, move(clonedChildren));
}

} // namespace evaluator
} // namespace graphflow
