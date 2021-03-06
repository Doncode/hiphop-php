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

#include <compiler/expression/dynamic_function_call.h>
#include <compiler/analysis/code_error.h>
#include <compiler/expression/expression_list.h>
#include <compiler/expression/scalar_expression.h>
#include <compiler/expression/simple_function_call.h>
#include <compiler/analysis/function_scope.h>
#include <compiler/analysis/class_scope.h>
#include <util/util.h>
#include <compiler/option.h>
#include <compiler/analysis/variable_table.h>

using namespace HPHP;

///////////////////////////////////////////////////////////////////////////////
// constructors/destructors

DynamicFunctionCall::DynamicFunctionCall
(EXPRESSION_CONSTRUCTOR_PARAMETERS,
 ExpressionPtr name, ExpressionListPtr params, ExpressionPtr cls)
  : FunctionCall(EXPRESSION_CONSTRUCTOR_PARAMETER_VALUES(DynamicFunctionCall),
                 name, "", params, cls) {
}

ExpressionPtr DynamicFunctionCall::clone() {
  DynamicFunctionCallPtr exp(new DynamicFunctionCall(*this));
  FunctionCall::deepCopy(exp);
  return exp;
}

///////////////////////////////////////////////////////////////////////////////
// parser functions

///////////////////////////////////////////////////////////////////////////////
// static analysis functions

void DynamicFunctionCall::analyzeProgram(AnalysisResultPtr ar) {
  FunctionCall::analyzeProgram(ar);
  if (ar->getPhase() >= AnalysisResult::AnalyzeAll) {
    if (!m_className.empty()) {
      resolveClass();
    }
    if (!m_class) {
      addUserClass(ar, m_className);
    }
    if (m_params) {
      m_params->markParams(canInvokeFewArgs());
    }

    if (!m_class && m_className.empty()) {
      FunctionScopePtr fs = getFunctionScope();
      VariableTablePtr vt = fs->getVariables();
      vt->setAttribute(VariableTable::ContainsDynamicFunctionCall);
    }
  }
}

ExpressionPtr DynamicFunctionCall::preOptimize(AnalysisResultConstPtr ar) {
  if (ExpressionPtr rep = FunctionCall::preOptimize(ar)) return rep;

  if (m_nameExp->isScalar()) {
    Variant v;
    if (m_nameExp->getScalarValue(v) &&
        v.isString()) {
      string name = v.toString().c_str();
      ExpressionPtr cls = m_class;
      if (!cls && !m_className.empty()) {
        cls = makeScalarExpression(ar, m_className);
      }
      return ExpressionPtr(NewSimpleFunctionCall(
        getScope(), getLocation(),
        name, m_params, cls));
    }
  }
  return ExpressionPtr();
}

TypePtr DynamicFunctionCall::inferTypes(AnalysisResultPtr ar, TypePtr type,
                                        bool coerce) {
  reset();
  ConstructPtr self = shared_from_this();
  if (m_class) {
    m_class->inferAndCheck(ar, Type::Any, false);
  } else if (!m_className.empty()) {
    ClassScopePtr cls = resolveClassWithChecks();
    if (cls) {
      m_classScope = cls;
    }
  }

  if (!m_class && m_className.empty()) {
    m_nameExp->inferAndCheck(ar, Type::Variant, false);
  } else {
    m_nameExp->inferAndCheck(ar, Type::String, false);
  }

  if (m_params) {
    for (int i = 0; i < m_params->getCount(); i++) {
      (*m_params)[i]->inferAndCheck(ar, Type::Variant, true);
    }
  }
  return Type::Variant;
}

///////////////////////////////////////////////////////////////////////////////
// code generation functions
void DynamicFunctionCall::outputPHP(CodeGenerator &cg, AnalysisResultPtr ar) {
  outputLineMap(cg, ar);

  if (m_class || !m_className.empty()) {
    StaticClassName::outputPHP(cg, ar);
    cg_printf("::");
    m_nameExp->outputPHP(cg, ar);
  } else {
    const char *prefix = Option::IdPrefix.c_str();
    if (cg.getOutput() == CodeGenerator::TrimmedPHP &&
        cg.usingStream(CodeGenerator::PrimaryStream) &&
        !m_nameExp->is(Expression::KindOfScalarExpression)) {
      cg_printf("${%sdynamic_load($%stmp = (", prefix, prefix);
      m_nameExp->outputPHP(cg, ar);
      cg_printf("), '%stmp'", prefix);
      cg_printf(")}");
    } else {
      m_nameExp->outputPHP(cg, ar);
    }
  }

  cg_printf("(");
  if (m_params) m_params->outputPHP(cg, ar);
  cg_printf(")");
}

bool DynamicFunctionCall::preOutputCPP(CodeGenerator &cg, AnalysisResultPtr ar,
    int state) {
  bool nonStatic = !m_class && m_className.empty();
  if (!nonStatic && !m_class && !m_classScope && !isRedeclared()) {
    // call to an unknown class
    // set m_noStatic to avoid pointlessly wrapping the call
    // in STATIC_CLASS_NAME_CALL()
    m_noStatic = true;
    cg.pushCallInfo(-1);
    bool ret = FunctionCall::preOutputCPP(cg, ar, state);
    cg.popCallInfo();
    return ret;
  }
  // Short circuit out if inExpression() returns false
  if (!cg.inExpression()) return true;

  cg.wrapExpressionBegin();

  m_ciTemp = cg.createNewLocalId(shared_from_this());

  if (!m_classScope && !m_className.empty() && m_cppTemp.empty() &&
      !isSelf() && ! isParent() && !isStatic()) {
    // Create a temporary to hold the class name, in case it is not a
    // StaticString.
    m_clsNameTemp = cg.createNewLocalId(shared_from_this());
    cg_printf("CStrRef clsName%d(", m_clsNameTemp);
    cg_printString(m_origClassName, ar, shared_from_this());
    cg_printf(");\n");
  }

  if (m_class) {
    int s = m_class->hasEffect() || m_nameExp->hasEffect() ?
      FixOrder : 0;
    m_class->preOutputCPP(cg, ar, s);
  }
  m_nameExp->preOutputCPP(cg, ar, 0);

  if (nonStatic) {
    cg_printf("const CallInfo *cit%d;\n", m_ciTemp);
    cg_printf("void *vt%d;\n", m_ciTemp);
    cg_printf("get_call_info_or_fail(cit%d, vt%d, ", m_ciTemp, m_ciTemp);

    if (m_nameExp->is(Expression::KindOfSimpleVariable)) {
      m_nameExp->outputCPP(cg, ar);
    } else {
      cg_printf("(");
      m_nameExp->outputCPP(cg, ar);
      cg_printf(")");
    }
    cg_printf(");\n");
  } else {
    cg_printf("MethodCallPackage mcp%d;\n", m_ciTemp);
    if (m_class) {
      if (m_class->is(KindOfScalarExpression)) {
        ASSERT(strcasecmp(dynamic_pointer_cast<ScalarExpression>(m_class)->
                          getString().c_str(), "static") == 0);
        cg_printf("CStrRef cls%d = "
                  "FrameInjection::GetStaticClassName(fi.getThreadInfo())",
                  m_ciTemp);
      } else {
        cg_printf("C%sRef cls%d = ",
                  m_class->getActualType() &&
                  m_class->getActualType()->is(Type::KindOfString) ?
                  "Str" : "Var", m_ciTemp);
        m_class->outputCPP(cg, ar);
      }
    } else if (m_classScope) {
      cg_printf("CStrRef cls%d = ", m_ciTemp);
      cg_printString(m_classScope->getId(), ar, shared_from_this());
    } else {
      cg_printf("CStrRef cls%d = ", m_ciTemp);
      cg_printString(m_className, ar, shared_from_this());
    }
    cg_printf(";\n");

    cg_printf("CStrRef mth%d = ", m_ciTemp);
    if (m_nameExp->is(Expression::KindOfSimpleVariable)) {
      m_nameExp->outputCPP(cg, ar);
    } else {
      cg_printf("(");
      m_nameExp->outputCPP(cg, ar);
      cg_printf(")");
    }
    cg_printf(";\n");

    bool dynamic = true;
    if (!m_class) {
      ClassScopeRawPtr origClass = getOriginalClass();
      if (!origClass) {
        dynamic = false;
      } else {
        FunctionScopeRawPtr origFunc = getOriginalFunction();
        if (origFunc) {
          if (origFunc->isStatic() ||
              (m_classScope != origClass &&
               (m_className.empty() || !origClass->derivesFrom(
                 ar, m_className, true, true)))) {
            dynamic = false;
          }
        }
      }
    }

    if (dynamic) {
      if ((m_class && (!m_class->getActualType() ||
                       !m_class->getActualType()->is(Type::KindOfString))) ||
          !getOriginalFunction() ||
          !getOriginalClass() ||
          getOriginalFunction()->isStatic()) {
        cg_printf("mcp%d.dynamicNamedCall(cls%d, mth%d);\n",
                  m_ciTemp, m_ciTemp, m_ciTemp);
      } else {
        cg_printf("mcp%d.isObj = true;\n", m_ciTemp);
        cg_printf("mcp%d.rootObj = this;\n", m_ciTemp);
        cg_printf("mcp%d.name = &mth%d;\n", m_ciTemp, m_ciTemp);
        cg_printf("o_get_call_info_ex(cls%d, mcp%d);\n", m_ciTemp, m_ciTemp);
      }
    } else {
      cg_printf("mcp%d.staticMethodCall(cls%d, mth%d);\n",
                m_ciTemp,
                m_ciTemp, m_ciTemp);

      if (m_classScope) {
        cg_printf("%s%s.%sget_call_info(mcp%d);\n",
                  Option::ClassStaticsCallbackPrefix,
                  m_classScope->getId().c_str(),
                  Option::ObjectStaticPrefix, m_ciTemp);
      } else if (isRedeclared()) {
        cg_printf("g->%s%s->%sget_call_info(mcp%d);\n",
                  Option::ClassStaticsCallbackPrefix, m_className.c_str(),
                  Option::ObjectStaticPrefix, m_ciTemp);
      } else {
        assert(false);
      }
    }
    cg_printf("const CallInfo *cit%d = mcp%d.ci;\n", m_ciTemp, m_ciTemp);
  }

  if (m_params && m_params->getCount() > 0) {
    cg.pushCallInfo(m_ciTemp);
    m_params->preOutputCPP(cg, ar, 0);
    cg.popCallInfo();
  }

  if (state & FixOrder) {
    cg.pushCallInfo(m_ciTemp);
    preOutputStash(cg, ar, state);
    cg.popCallInfo();
  }

  return true;
}

void DynamicFunctionCall::outputCPPImpl(CodeGenerator &cg,
                                        AnalysisResultPtr ar) {
  bool method = false;
  if (m_class || !m_className.empty()) {
    if (!m_class && !m_classScope && !isRedeclared()) {
      const string &name = CodeGenerator::EscapeLabel(m_className);
      cg_printf("throw_fatal(\"unknown class %s\")", name.c_str());
      return;
    }
    method = true;
  }

  cg_printf("(cit%d->", m_ciTemp);
  outputDynamicCall(cg, ar, method);
}
