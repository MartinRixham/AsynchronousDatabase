# The stack to act on. The pipeline stands up several at once — one per parallel share of the chaos
# suite — and they differ in nothing but the name.
STACK ?= asyncdb

create-stack:
	aws cloudformation create-stack --stack-name $(STACK) --template-body file://cloudformation.yaml --capabilities CAPABILITY_NAMED_IAM

update-stack:
	aws cloudformation update-stack --stack-name $(STACK) --template-body file://cloudformation.yaml --capabilities CAPABILITY_NAMED_IAM

delete-stack:
	aws cloudformation delete-stack --stack-name $(STACK)

describe-stack:
	aws cloudformation describe-stack-events --stack-name $(STACK)

create-chaos-stack:
	aws cloudformation create-stack --stack-name asyncdb-chaos --template-body file://chaos/chaos.yaml --capabilities CAPABILITY_IAM

update-chaos-stack:
	aws cloudformation update-stack --stack-name asyncdb-chaos --template-body file://chaos/chaos.yaml --capabilities CAPABILITY_IAM

delete-chaos-stack:
	aws cloudformation delete-stack --stack-name asyncdb-chaos
