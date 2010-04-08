#include "eval.hh"
#include "parser.hh"
#include "hash.hh"
#include "util.hh"
#include "store-api.hh"
#include "derivations.hh"
#include "nixexpr-ast.hh"
#include "globals.hh"

#include <cstring>


#define LocalNoInline(f) static f __attribute__((noinline)); f
#define LocalNoInlineNoReturn(f) static f __attribute__((noinline, noreturn)); f


namespace nix {
    

std::ostream & operator << (std::ostream & str, Value & v)
{
    switch (v.type) {
    case tInt:
        str << v.integer;
        break;
    case tBool:
        str << (v.boolean ? "true" : "false");
        break;
    case tString:
        str << "\"";
        for (const char * i = v.string.s; *i; i++)
            if (*i == '\"' || *i == '\\') str << "\\" << *i;
            else if (*i == '\n') str << "\\n";
            else if (*i == '\r') str << "\\r";
            else if (*i == '\t') str << "\\t";
            else str << *i;
        str << "\"";
        break;
    case tPath:
        str << v.path; // !!! escaping?
        break;
    case tNull:
        str << "true";
        break;
    case tAttrs:
        str << "{ ";
        foreach (Bindings::iterator, i, *v.attrs)
            str << aterm2String(i->first) << " = " << i->second << "; ";
        str << "}";
        break;
    case tList:
        str << "[ ";
        for (unsigned int n = 0; n < v.list.length; ++n)
            str << v.list.elems[n] << " ";
        str << "]";
        break;
    case tThunk:
        str << "<CODE>";
        break;
    case tLambda:
        str << "<LAMBDA>";
        break;
    case tPrimOp:
        str << "<PRIMOP>";
        break;
    case tPrimOpApp:
        str << "<PRIMOP-APP>";
        break;
    default:
        throw Error("invalid value");
    }
    return str;
}


string showType(Value & v)
{
    switch (v.type) {
        case tInt: return "an integer";
        case tBool: return "a boolean";
        case tString: return "a string";
        case tPath: return "a path";
        case tAttrs: return "an attribute set";
        case tList: return "a list";
        case tNull: return "null";
        case tLambda: return "a function";
        case tPrimOp: return "a built-in function";
        case tPrimOpApp: return "a partially applied built-in function";
        default: throw Error(format("unknown type: %1%") % v.type);
    }
}


EvalState::EvalState() : baseEnv(allocEnv())
{
    nrValues = nrEnvs = nrEvaluated = 0;

    initNixExprHelpers();

    createBaseEnv();
    
    allowUnsafeEquality = getEnv("NIX_NO_UNSAFE_EQ", "") == "";
}


void EvalState::addConstant(const string & name, Value & v)
{
    baseEnv.bindings[toATerm(name)] = v;
    string name2 = string(name, 0, 2) == "__" ? string(name, 2) : name;
    (*baseEnv.bindings[toATerm("builtins")].attrs)[toATerm(name2)] = v;
    nrValues += 2;
}


void EvalState::addPrimOp(const string & name,
    unsigned int arity, PrimOp primOp)
{
    Value v;
    v.type = tPrimOp;
    v.primOp.arity = arity;
    v.primOp.fun = primOp;
    baseEnv.bindings[toATerm(name)] = v;
    string name2 = string(name, 0, 2) == "__" ? string(name, 2) : name;
    (*baseEnv.bindings[toATerm("builtins")].attrs)[toATerm(name2)] = v;
    nrValues += 2;
}


/* Every "format" object (even temporary) takes up a few hundred bytes
   of stack space, which is a real killer in the recursive
   evaluator.  So here are some helper functions for throwing
   exceptions. */

LocalNoInlineNoReturn(void throwEvalError(const char * s))
{
    throw EvalError(s);
}

LocalNoInlineNoReturn(void throwEvalError(const char * s, const string & s2))
{
    throw EvalError(format(s) % s2);
}

LocalNoInlineNoReturn(void throwTypeError(const char * s))
{
    throw TypeError(s);
}

LocalNoInlineNoReturn(void throwTypeError(const char * s, const string & s2))
{
    throw TypeError(format(s) % s2);
}

LocalNoInline(void addErrorPrefix(Error & e, const char * s))
{
    e.addPrefix(s);
}

LocalNoInline(void addErrorPrefix(Error & e, const char * s, const string & s2))
{
    e.addPrefix(format(s) % s2);
}

LocalNoInline(void addErrorPrefix(Error & e, const char * s, const string & s2, const string & s3))
{
    e.addPrefix(format(s) % s2 % s3);
}


void mkString(Value & v, const char * s)
{
    v.type = tString;
    v.string.s = strdup(s);
    v.string.context = 0;
}


void mkString(Value & v, const string & s, const PathSet & context)
{
    mkString(v, s.c_str());
    if (!context.empty()) {
        unsigned int n = 0;
        v.string.context = new const char *[context.size() + 1];
        foreach (PathSet::const_iterator, i, context) 
            v.string.context[n++] = strdup(i->c_str());
        v.string.context[n] = 0;
    }
}


void mkPath(Value & v, const char * s)
{
    v.type = tPath;
    v.path = strdup(s);
}


static Value * lookupWith(Env * env, Sym name)
{
    if (!env) return 0;
    Value * v = lookupWith(env->up, name);
    if (v) return v;
    Bindings::iterator i = env->bindings.find(sWith);
    if (i == env->bindings.end()) return 0;
    Bindings::iterator j = i->second.attrs->find(name);
    if (j != i->second.attrs->end()) return &j->second;
    return 0;
}


static Value * lookupVar(Env * env, Sym name)
{
    /* First look for a regular variable binding for `name'. */
    for (Env * env2 = env; env2; env2 = env2->up) {
        Bindings::iterator i = env2->bindings.find(name);
        if (i != env2->bindings.end()) return &i->second;
    }

    /* Otherwise, look for a `with' attribute set containing `name'.
       Outer `withs' take precedence (i.e. `with {x=1;}; with {x=2;};
       x' evaluates to 1).  */
    Value * v = lookupWith(env, name);
    if (v) return v;

    /* Alternative implementation where the inner `withs' take
       precedence (i.e. `with {x=1;}; with {x=2;}; x' evaluates to
       2). */
#if 0
    for (Env * env2 = env; env2; env2 = env2->up) {
        Bindings::iterator i = env2->bindings.find(sWith);
        if (i == env2->bindings.end()) continue;
        Bindings::iterator j = i->second.attrs->find(name);
        if (j != i->second.attrs->end()) return &j->second;
    }
#endif
    
    throw Error(format("undefined variable `%1%'") % aterm2String(name));
}


Value * EvalState::allocValues(unsigned int count)
{
    nrValues += count;
    return new Value[count]; // !!! check destructor
}


Env & EvalState::allocEnv()
{
    nrEnvs++;
    return *(new Env);
}


void EvalState::mkList(Value & v, unsigned int length)
{
    v.type = tList;
    v.list.length = length;
    v.list.elems = allocValues(length);
}


void EvalState::mkAttrs(Value & v)
{
    v.type = tAttrs;
    v.attrs = new Bindings;
}


void EvalState::mkThunk_(Value & v, Expr expr)
{
    mkThunk(v, baseEnv, expr);
}


void EvalState::cloneAttrs(Value & src, Value & dst)
{
    mkAttrs(dst);
    foreach (Bindings::iterator, i, *src.attrs)
        mkCopy((*dst.attrs)[i->first], i->second);
}


void EvalState::evalFile(const Path & path, Value & v)
{
    startNest(nest, lvlTalkative, format("evaluating file `%1%'") % path);

    Expr e = parseTrees.get(toATerm(path));

    if (!e) {
        e = parseExprFromFile(*this, path);
        parseTrees.set(toATerm(path), e);
    }
    
    try {
        eval(e, v);
    } catch (Error & e) {
        e.addPrefix(format("while evaluating the file `%1%':\n")
            % path);
        throw;
    }
}


static char * deepestStack = (char *) -1; /* for measuring stack usage */


void EvalState::eval(Env & env, Expr e, Value & v)
{
    /* When changing this function, make sure that you don't cause a
       (large) increase in stack consumption! */
    
    char x;
    if (&x < deepestStack) deepestStack = &x;
    
    //debug(format("eval: %1%") % e);

    checkInterrupt();

    nrEvaluated++;

    Sym name;
    int n;
    ATerm s; ATermList context, es;
    ATermList rbnds, nrbnds;
    Expr e1, e2, e3, fun, arg, attrs;
    Pattern pat; Expr body; Pos pos;
    
    if (matchVar(e, name)) {
        Value * v2 = lookupVar(&env, name);
        forceValue(*v2);
        v = *v2;
    }

    else if (matchInt(e, n))
        mkInt(v, n);

    else if (matchStr(e, s, context)) {
        assert(context == ATempty);
        mkString(v, ATgetName(ATgetAFun(s)));
    }

    else if (matchPath(e, s))
        mkPath(v, ATgetName(ATgetAFun(s)));

    else if (matchAttrs(e, es)) {
        mkAttrs(v);
        ATerm e2, pos;
        for (ATermIterator i(es); i; ++i) {
            if (!matchBind(*i, name, e2, pos)) abort(); /* can't happen */
            Value & v2 = (*v.attrs)[name];
            nrValues++;
            mkThunk(v2, env, e2);
        }
    }

    else if (matchRec(e, rbnds, nrbnds)) {
        /* Create a new environment that contains the attributes in
           this `rec'. */
        Env & env2(allocEnv());
        env2.up = &env;
        
        v.type = tAttrs;
        v.attrs = &env2.bindings;

        /* The recursive attributes are evaluated in the new
           environment. */
        ATerm name, e2, pos;
        for (ATermIterator i(rbnds); i; ++i) {
            if (!matchBind(*i, name, e2, pos)) abort(); /* can't happen */
            Value & v2 = env2.bindings[name];
            nrValues++;
            mkThunk(v2, env2, e2);
        }

        /* The non-recursive attributes, on the other hand, are
           evaluated in the original environment. */
        for (ATermIterator i(nrbnds); i; ++i) {
            if (!matchBind(*i, name, e2, pos)) abort(); /* can't happen */
            Value & v2 = env2.bindings[name];
            nrValues++;
            mkThunk(v2, env, e2);
        }
    }

    else if (matchSelect(e, e2, name)) {
        Value v2;
        eval(env, e2, v2);
        forceAttrs(v2); // !!! eval followed by force is slightly inefficient
        Bindings::iterator i = v2.attrs->find(name);
        if (i == v2.attrs->end())
            throwEvalError("attribute `%1%' missing", aterm2String(name));
        try {            
            forceValue(i->second);
        } catch (Error & e) {
            addErrorPrefix(e, "while evaluating the attribute `%1%':\n", aterm2String(name));
            throw;
        }
        v = i->second;
    }

    else if (matchFunction(e, pat, body, pos)) {
        v.type = tLambda;
        v.lambda.env = &env;
        v.lambda.pat = pat;
        v.lambda.body = body;
    }

    else if (matchCall(e, fun, arg)) {
        Value vFun;
        eval(env, fun, vFun);
        Value vArg;
        mkThunk(vArg, env, arg); // !!! should this be on the heap?
        callFunction(vFun, vArg, v);
    }

    else if (matchWith(e, attrs, body, pos)) {
        Env & env2(allocEnv());
        env2.up = &env;

        Value & vAttrs = env2.bindings[sWith];
        nrValues++;
        eval(env, attrs, vAttrs);
        forceAttrs(vAttrs);
        
        eval(env2, body, v);
    }

    else if (matchList(e, es)) {
        mkList(v, ATgetLength(es));
        for (unsigned int n = 0; n < v.list.length; ++n, es = ATgetNext(es))
            mkThunk(v.list.elems[n], env, ATgetFirst(es));
    }

    else if (matchOpEq(e, e1, e2)) {
        Value v1; eval(env, e1, v1);
        Value v2; eval(env, e2, v2);
        mkBool(v, eqValues(v1, v2));
    }

    else if (matchOpNEq(e, e1, e2)) {
        Value v1; eval(env, e1, v1);
        Value v2; eval(env, e2, v2);
        mkBool(v, !eqValues(v1, v2));
    }

    else if (matchOpConcat(e, e1, e2)) {
        Value v1; eval(env, e1, v1);
        forceList(v1);
        Value v2; eval(env, e2, v2);
        forceList(v2);
        mkList(v, v1.list.length + v2.list.length);
        /* !!! This loses sharing with the original lists.  We could
           use a tCopy node, but that would use more memory. */
        for (unsigned int n = 0; n < v1.list.length; ++n)
            v.list.elems[n] = v1.list.elems[n];
        for (unsigned int n = 0; n < v2.list.length; ++n)
            v.list.elems[n + v1.list.length] = v2.list.elems[n];
    }

    else if (matchConcatStrings(e, es)) {
        PathSet context;
        std::ostringstream s;
        
        bool first = true, isPath = false;
        Value vStr;
        
        for (ATermIterator i(es); i; ++i) {
            eval(env, *i, vStr);

            /* If the first element is a path, then the result will
               also be a path, we don't copy anything (yet - that's
               done later, since paths are copied when they are used
               in a derivation), and none of the strings are allowed
               to have contexts. */
            if (first) {
                isPath = vStr.type == tPath;
                first = false;
            }
            
            s << coerceToString(vStr, context, false, !isPath);
        }
        
        if (isPath && !context.empty())
            throw EvalError(format("a string that refers to a store path cannot be appended to a path, in `%1%'")
                % s.str());

        if (isPath)
            mkPath(v, s.str().c_str());
        else
            mkString(v, s.str(), context);
    }

    /* Conditionals. */
    else if (matchIf(e, e1, e2, e3))
        eval(env, evalBool(env, e1) ? e2 : e3, v);

    /* Assertions. */
    else if (matchAssert(e, e1, e2, pos)) {
        if (!evalBool(env, e1))
            throw AssertionError(format("assertion failed at %1%") % showPos(pos));
        eval(env, e2, v);
    }

    /* Negation. */
    else if (matchOpNot(e, e1))
        mkBool(v, !evalBool(env, e1));

    /* Implication. */
    else if (matchOpImpl(e, e1, e2))
        return mkBool(v, !evalBool(env, e1) || evalBool(env, e2));
    
    /* Conjunction (logical AND). */
    else if (matchOpAnd(e, e1, e2))
        mkBool(v, evalBool(env, e1) && evalBool(env, e2));
    
    /* Disjunction (logical OR). */
    else if (matchOpOr(e, e1, e2))
        mkBool(v, evalBool(env, e1) || evalBool(env, e2));

    /* Attribute set update (//). */
    else if (matchOpUpdate(e, e1, e2)) {
        Value v2;
        eval(env, e1, v2);
        
        cloneAttrs(v2, v);
        
        eval(env, e2, v2);
        foreach (Bindings::iterator, i, *v2.attrs)
            (*v.attrs)[i->first] = i->second; // !!! sharing
    }

    /* Attribute existence test (?). */
    else if (matchOpHasAttr(e, e1, name)) {
        Value vAttrs;
        eval(env, e1, vAttrs);
        forceAttrs(vAttrs);
        mkBool(v, vAttrs.attrs->find(name) != vAttrs.attrs->end());
    }

    else throw Error("unsupported term");
}


void EvalState::callFunction(Value & fun, Value & arg, Value & v)
{
    if (fun.type == tPrimOp || fun.type == tPrimOpApp) {
        unsigned int argsLeft =
            fun.type == tPrimOp ? fun.primOp.arity : fun.primOpApp.argsLeft;
        if (argsLeft == 1) {
            /* We have all the arguments, so call the primop.  First
               find the primop. */
            Value * primOp = &fun;
            while (primOp->type == tPrimOpApp) primOp = primOp->primOpApp.left;
            assert(primOp->type == tPrimOp);
            unsigned int arity = primOp->primOp.arity;
                
            /* Put all the arguments in an array. */
            Value * vArgs[arity];
            unsigned int n = arity - 1;
            vArgs[n--] = &arg;
            for (Value * arg = &fun; arg->type == tPrimOpApp; arg = arg->primOpApp.left)
                vArgs[n--] = arg->primOpApp.right;

            /* And call the primop. */
            primOp->primOp.fun(*this, vArgs, v);
        } else {
            Value * v2 = allocValues(2);
            v2[0] = fun;
            v2[1] = arg;
            v.type = tPrimOpApp;
            v.primOpApp.left = &v2[0];
            v.primOpApp.right = &v2[1];
            v.primOpApp.argsLeft = argsLeft - 1;
        }
        return;
    }
    
    if (fun.type != tLambda)
        throwTypeError("attempt to call something which is neither a function nor a primop (built-in operation) but %1%",
            showType(fun));

    Env & env2(allocEnv());
    env2.up = fun.lambda.env;

    ATermList formals; ATerm ellipsis, name;

    if (matchVarPat(fun.lambda.pat, name)) {
        Value & vArg = env2.bindings[name];
        nrValues++;
        vArg = arg;
    }

    else if (matchAttrsPat(fun.lambda.pat, formals, ellipsis, name)) {
        forceAttrs(arg);
        
        if (name != sNoAlias) {
            env2.bindings[name] = arg;
            nrValues++;
        }                

        /* For each formal argument, get the actual argument.  If
           there is no matching actual argument but the formal
           argument has a default, use the default. */
        unsigned int attrsUsed = 0;
        for (ATermIterator i(formals); i; ++i) {
            Expr def; Sym name;
            DefaultValue def2;
            if (!matchFormal(*i, name, def2)) abort(); /* can't happen */

            Bindings::iterator j = arg.attrs->find(name);
                
            Value & v = env2.bindings[name];
            nrValues++;
                
            if (j == arg.attrs->end()) {
                if (!matchDefaultValue(def2, def)) def = 0;
                if (def == 0) throw TypeError(format("the argument named `%1%' required by the function is missing")    
                    % aterm2String(name));
                mkThunk(v, env2, def);
            } else {
                attrsUsed++;
                mkCopy(v, j->second);
            }
        }

        /* Check that each actual argument is listed as a formal
           argument (unless the attribute match specifies a `...').
           TODO: show the names of the expected/unexpected
           arguments. */
        if (ellipsis == eFalse && attrsUsed != arg.attrs->size())
            throw TypeError("function called with unexpected argument");
    }

    else abort();
        
    eval(env2, fun.lambda.body, v);
}


void EvalState::autoCallFunction(const Bindings & args, Value & fun, Value & res)
{
    forceValue(fun);

    ATerm name;
    ATermList formals;
    ATermBool ellipsis;
    
    if (fun.type != tLambda || !matchAttrsPat(fun.lambda.pat, formals, ellipsis, name)) {
        res = fun;
        return;
    }

    Value actualArgs;
    mkAttrs(actualArgs);
    
    for (ATermIterator i(formals); i; ++i) {
        Expr name, def; ATerm def2;
        if (!matchFormal(*i, name, def2)) abort();
        Bindings::const_iterator j = args.find(name);
        if (j != args.end())
            (*actualArgs.attrs)[name] = j->second;
        else if (!matchDefaultValue(def2, def))
            throw TypeError(format("cannot auto-call a function that has an argument without a default value (`%1%      ')")
                % aterm2String(name));
    }

    callFunction(fun, actualArgs, res);
}


void EvalState::eval(Expr e, Value & v)
{
    eval(baseEnv, e, v);
}


bool EvalState::evalBool(Env & env, Expr e)
{
    Value v;
    eval(env, e, v);
    if (v.type != tBool)
        throw TypeError(format("value is %1% while a Boolean was expected") % showType(v));
    return v.boolean;
}


void EvalState::forceValue(Value & v)
{
    if (v.type == tThunk) {
        ValueType saved = v.type;
        try {
            v.type = tBlackhole;
            eval(*v.thunk.env, v.thunk.expr, v);
        } catch (Error & e) {
            v.type = saved;
            throw;
        }
    }
    else if (v.type == tCopy) {
        forceValue(*v.val);
        v = *v.val;
    }
    else if (v.type == tApp)
        callFunction(*v.app.left, *v.app.right, v);
    else if (v.type == tBlackhole)
        throw EvalError("infinite recursion encountered");
}


void EvalState::strictForceValue(Value & v)
{
    forceValue(v);
    
    if (v.type == tAttrs) {
        foreach (Bindings::iterator, i, *v.attrs)
            strictForceValue(i->second);
    }
    
    else if (v.type == tList) {
        for (unsigned int n = 0; n < v.list.length; ++n)
            strictForceValue(v.list.elems[n]);
    }
}


int EvalState::forceInt(Value & v)
{
    forceValue(v);
    if (v.type != tInt)
        throw TypeError(format("value is %1% while an integer was expected") % showType(v));
    return v.integer;
}


bool EvalState::forceBool(Value & v)
{
    forceValue(v);
    if (v.type != tBool)
        throw TypeError(format("value is %1% while a Boolean was expected") % showType(v));
    return v.boolean;
}


void EvalState::forceAttrs(Value & v)
{
    forceValue(v);
    if (v.type != tAttrs)
        throw TypeError(format("value is %1% while an attribute set was expected") % showType(v));
}


void EvalState::forceList(Value & v)
{
    forceValue(v);
    if (v.type != tList)
        throw TypeError(format("value is %1% while a list was expected") % showType(v));
}


void EvalState::forceFunction(Value & v)
{
    forceValue(v);
    if (v.type != tLambda && v.type != tPrimOp && v.type != tPrimOpApp)
        throw TypeError(format("value is %1% while a function was expected") % showType(v));
}


string EvalState::forceString(Value & v)
{
    forceValue(v);
    if (v.type != tString)
        throw TypeError(format("value is %1% while a string was expected") % showType(v));
    return string(v.string.s);
}


string EvalState::forceString(Value & v, PathSet & context)
{
    string s = forceString(v);
    if (v.string.context) {
        for (const char * * p = v.string.context; *p; ++p) 
            context.insert(*p);
    }
    return s;
}


string EvalState::forceStringNoCtx(Value & v)
{
    string s = forceString(v);
    if (v.string.context)
        throw EvalError(format("the string `%1%' is not allowed to refer to a store path (such as `%2%')")
            % v.string.s % v.string.context[0]);
    return s;
}


bool EvalState::isDerivation(Value & v)
{
    if (v.type != tAttrs) return false;
    Bindings::iterator i = v.attrs->find(toATerm("type"));
    return i != v.attrs->end() && forceStringNoCtx(i->second) == "derivation";
}


string EvalState::coerceToString(Value & v, PathSet & context,
    bool coerceMore, bool copyToStore)
{
    forceValue(v);

    string s;

    if (v.type == tString) {
        if (v.string.context) 
            for (const char * * p = v.string.context; *p; ++p) 
                context.insert(*p);
        return v.string.s;
    }

    if (v.type == tPath) {
        Path path(canonPath(v.path));

        if (!copyToStore) return path;
        
        if (nix::isDerivation(path))
            throw EvalError(format("file names are not allowed to end in `%1%'")
                % drvExtension);

        Path dstPath;
        if (srcToStore[path] != "")
            dstPath = srcToStore[path];
        else {
            dstPath = readOnlyMode
                ? computeStorePathForPath(path).first
                : store->addToStore(path);
            srcToStore[path] = dstPath;
            printMsg(lvlChatty, format("copied source `%1%' -> `%2%'")
                % path % dstPath);
        }

        context.insert(dstPath);
        return dstPath;
    }

    if (v.type == tAttrs) {
        Bindings::iterator i = v.attrs->find(toATerm("outPath"));
        if (i == v.attrs->end())
            throwTypeError("cannot coerce an attribute set (except a derivation) to a string");
        return coerceToString(i->second, context, coerceMore, copyToStore);
    }

    if (coerceMore) {

        /* Note that `false' is represented as an empty string for
           shell scripting convenience, just like `null'. */
        if (v.type == tBool && v.boolean) return "1";
        if (v.type == tBool && !v.boolean) return "";
        if (v.type == tInt) return int2String(v.integer);
        if (v.type == tNull) return "";

        if (v.type == tList) {
            string result;
            for (unsigned int n = 0; n < v.list.length; ++n) {
                result += coerceToString(v.list.elems[n],
                    context, coerceMore, copyToStore);
                if (n < v.list.length - 1
                    /* !!! not quite correct */
                    && (v.list.elems[n].type != tList || v.list.elems[n].list.length != 0))
                    result += " ";
            }
            return result;
        }
    }
    
    throwTypeError("cannot coerce %1% to a string", showType(v));
}


Path EvalState::coerceToPath(Value & v, PathSet & context)
{
    string path = coerceToString(v, context, false, false);
    if (path == "" || path[0] != '/')
        throw EvalError(format("string `%1%' doesn't represent an absolute path") % path);
    return path;
}


bool EvalState::eqValues(Value & v1, Value & v2)
{
    forceValue(v1);
    forceValue(v2);

    if (v1.type != v2.type) return false;
    
    switch (v1.type) {

        case tInt:
            return v1.integer == v2.integer;

        case tBool:
            return v1.boolean == v2.boolean;

        case tString:
            /* !!! contexts */
            return strcmp(v1.string.s, v2.string.s) == 0;

        case tPath:
            return strcmp(v1.path, v2.path) == 0;

        case tNull:
            return true;

        case tList:
            if (v2.type != tList || v1.list.length != v2.list.length) return false;
            for (unsigned int n = 0; n < v1.list.length; ++n)
                if (!eqValues(v1.list.elems[n], v2.list.elems[n])) return false;
            return true;

        case tAttrs: {
            if (v2.type != tAttrs || v1.attrs->size() != v2.attrs->size()) return false;
            Bindings::iterator i, j;
            for (i = v1.attrs->begin(), j = v2.attrs->begin(); i != v1.attrs->end(); ++i, ++j)
                if (!eqValues(i->second, j->second)) return false;
            return true;
        }

        /* Functions are incomparable. */
        case tLambda:
        case tPrimOp:
        case tPrimOpApp:
            return false;

        default:
            throw Error(format("cannot compare %1% with %2%") % showType(v1) % showType(v2));
    }
}


void EvalState::printStats()
{
    char x;
    bool showStats = getEnv("NIX_SHOW_STATS", "0") != "0";
    printMsg(showStats ? lvlInfo : lvlDebug,
        format("evaluated %1% expressions, used %2% bytes of stack space, allocated %3% values, allocated %4% environments")
        % nrEvaluated
        % (&x - deepestStack)
        % nrValues
        % nrEnvs);
    if (showStats)
        printATermMapStats();
}


}
