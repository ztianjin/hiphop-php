/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include <compiler/expression/closure_expression.h>
#include <compiler/expression/parameter_expression.h>
#include <compiler/expression/expression_list.h>
#include <compiler/expression/simple_variable.h>
#include <compiler/statement/function_statement.h>
#include <compiler/analysis/variable_table.h>
#include <compiler/analysis/function_scope.h>

using namespace HPHP;
using namespace std;
using namespace boost;

///////////////////////////////////////////////////////////////////////////////
// constructors/destructors

ClosureExpression::ClosureExpression
(EXPRESSION_CONSTRUCTOR_PARAMETERS, FunctionStatementPtr func,
 ExpressionListPtr vars)
    : Expression(EXPRESSION_CONSTRUCTOR_PARAMETER_VALUES),
      m_func(func) {

  if (vars) {
    m_vars = ExpressionListPtr
      (new ExpressionList(vars->getScope(), vars->getLocation(),
                          KindOfExpressionList));
    // push the vars in reverse order, not retaining duplicates
    set<string> seenBefore;
    for (int i = vars->getCount() - 1; i >= 0; i--) {
      ParameterExpressionPtr param(
        dynamic_pointer_cast<ParameterExpression>((*vars)[i]));
      ASSERT(param);
      if (seenBefore.find(param->getName().c_str()) == seenBefore.end()) {
        seenBefore.insert(param->getName().c_str());
        m_vars->insertElement(param);
      }
    }

    if (m_vars) {
      m_values = ExpressionListPtr
        (new ExpressionList(m_vars->getScope(), m_vars->getLocation(),
                            KindOfExpressionList));
      for (int i = 0; i < m_vars->getCount(); i++) {
        ParameterExpressionPtr param =
          dynamic_pointer_cast<ParameterExpression>((*m_vars)[i]);
        string name = param->getName();

        SimpleVariablePtr var(new SimpleVariable(param->getScope(),
                                                 param->getLocation(),
                                                 KindOfSimpleVariable,
                                                 name));
        if (param->isRef()) {
          var->setContext(RefValue);
        }
        m_values->addElement(var);
      }
    }
  }
}

ExpressionPtr ClosureExpression::clone() {
  ClosureExpressionPtr exp(new ClosureExpression(*this));
  Expression::deepCopy(exp);
  exp->m_func = Clone(m_func);
  exp->m_vars = Clone(m_vars);
  exp->m_values = Clone(m_values);
  return exp;
}

ConstructPtr ClosureExpression::getNthKid(int n) const {
  switch (n) {
    case 0:
      return m_values;
    default:
      ASSERT(false);
      break;
  }
  return ConstructPtr();
}

int ClosureExpression::getKidCount() const {
  return 1;
}

void ClosureExpression::setNthKid(int n, ConstructPtr cp) {
  switch (n) {
    case 0:
      m_values = boost::dynamic_pointer_cast<ExpressionList>(cp);
      break;
    default:
      ASSERT(false);
      break;
  }
}

///////////////////////////////////////////////////////////////////////////////
// parser functions

///////////////////////////////////////////////////////////////////////////////
// static analysis functions

void ClosureExpression::analyzeProgram(AnalysisResultPtr ar) {
  m_func->analyzeProgram(ar);

  if (m_vars) {
    m_values->analyzeProgram(ar);

    if (ar->getPhase() == AnalysisResult::AnalyzeAll) {
      m_func->getFunctionScope()->setClosureVars(m_vars);

      // closure function's variable table (not containing function's)
      VariableTablePtr variables = m_func->getFunctionScope()->getVariables();
      for (int i = 0; i < m_vars->getCount(); i++) {
        ParameterExpressionPtr param =
          dynamic_pointer_cast<ParameterExpression>((*m_vars)[i]);
        string name = param->getName();
        {
          Symbol *sym = variables->addSymbol(name);
          sym->setClosureVar();
          if (param->isRef()) {
            sym->setRefClosureVar();
          } else {
            sym->clearRefClosureVar();
          }
        }
      }
      return;
    }
    if (ar->getPhase() == AnalysisResult::AnalyzeFinal) {
      // closure function's variable table (not containing function's)
      VariableTablePtr variables = m_func->getFunctionScope()->getVariables();
      for (int i = 0; i < m_vars->getCount(); i++) {
        ParameterExpressionPtr param =
          dynamic_pointer_cast<ParameterExpression>((*m_vars)[i]);
        string name = param->getName();

        // so we can assign values to them, instead of seeing CVarRef
        Symbol *sym = variables->getSymbol(name);
        if (sym && sym->isParameter()) {
          sym->setLvalParam();
        }
      }
    }
  }
}

TypePtr ClosureExpression::inferTypes(AnalysisResultPtr ar, TypePtr type,
                                      bool coerce) {
  m_func->inferTypes(ar);
  if (m_values) m_values->inferAndCheck(ar, Type::Some, false);
  if (m_vars) {
    // containing function's variable table (not closure function's)
    VariableTablePtr variables = getScope()->getVariables();
    for (int i = 0; i < m_vars->getCount(); i++) {
      ParameterExpressionPtr param =
        dynamic_pointer_cast<ParameterExpression>((*m_vars)[i]);
      string name = param->getName();
      if (param->isRef()) {
        variables->forceVariant(ar, name, VariableTable::AnyVars);
      }
    }
  }
  return Type::CreateObjectType("closure"); // needs lower case
}

///////////////////////////////////////////////////////////////////////////////
// code generation functions

void ClosureExpression::outputPHP(CodeGenerator &cg, AnalysisResultPtr ar) {
  m_func->outputPHPHeader(cg, ar);
  if (m_vars && m_vars->getCount()) {
    cg_printf(" use (");
    m_vars->outputPHP(cg, ar);
    cg_printf(")");
  }
  m_func->outputPHPBody(cg, ar);
}

void ClosureExpression::outputCPPImpl(CodeGenerator &cg,
                                      AnalysisResultPtr ar) {


  FunctionScopeRawPtr cfunc(m_func->getFunctionScope());
  VariableTablePtr vt(cfunc->getVariables());
  ParameterExpressionPtrIdxPairVec useVars;
  bool needsAnonCls = cfunc->needsAnonClosureClass(useVars);

  const string &origName = m_func->getOriginalName();

  if (needsAnonCls) {
    cg_printf("%sClosure$%s(NEWOBJ(%sClosure$%s)(&%s%s, NULL, ",
              Option::SmartPtrPrefix, origName.c_str(),
              Option::ClassPrefix, origName.c_str(),
              Option::CallInfoPrefix, origName.c_str());
  } else if (cfunc->isClosureGenerator()) {
    cg_printf("%sGeneratorClosure(NEWOBJ(%sGeneratorClosure)(&%s%s, NULL, ",
              Option::SmartPtrPrefix, Option::ClassPrefix,
              Option::CallInfoPrefix, origName.c_str());
  } else {
    // no use vars, so can use the generic closure
    cg_printf("%sClosure(NEWOBJ(%sClosure)(&%s%s, NULL",
              Option::SmartPtrPrefix, Option::ClassPrefix,
              Option::CallInfoPrefix, origName.c_str());
  }

  bool hasEmit = false;
  if (needsAnonCls) {
    ASSERT(m_vars && m_vars->getCount());
    BOOST_FOREACH(ParameterExpressionPtrIdxPair paramPair, useVars) {
      ParameterExpressionPtr param(paramPair.first);
      string name = param->getName();
      ExpressionPtr value((*m_values)[paramPair.second]);
      if (!hasEmit) hasEmit = true;
      else          cg_printf(", ");
      bool ref = param->isRef() && value->isRefable();
      if (ref) {
        value->setContext(NoRefWrapper);
        cg_printf("strongBind(");
      }
      value->outputCPP(cg, ar);
      if (ref) cg_printf(")");
    }
  } else if (cfunc->isClosureGenerator()) {
    // TODO: for some reason, use vars which are also params in
    // closure generators do not get the "present" bit set,
    // so we cannot filter present symbols. That is why
    // we fetch the closure use vars again (w/o the filter)
    useVars.clear();
    cfunc->getClosureUseVars(useVars, false);
    if (useVars.size()) {
      ASSERT(m_vars && m_vars->getCount());
      cg_printf("Array(ArrayInit(%d, true)", useVars.size());
      BOOST_FOREACH(ParameterExpressionPtrIdxPair paramPair, useVars) {
        ParameterExpressionPtr param(paramPair.first);
        ExpressionPtr value((*m_values)[paramPair.second]);
        bool ref = param->isRef() && value->isRefable();
        if (ref) value->setContext(NoRefWrapper);
        cg_printf(".set%s(\"%s\", ", ref ? "Ref" : "", param->getName().c_str());
        value->outputCPP(cg, ar);
        cg_printf(")");
      }
      cg_printf(".create())");
    } else {
      cg_printf("Array()");
    }
  }

  cg_printf("))");
}
