create-stack:
	aws cloudformation create-stack --stack-name asyncdb --template-body file://cloudformation.json --capabilities CAPABILITY_NAMED_IAM

update-stack:
	aws cloudformation update-stack --stack-name asyncdb --template-body file://cloudformation.json --capabilities CAPABILITY_NAMED_IAM

delete-stack:
	aws cloudformation delete-stack --stack-name asyncdb

describe-stack:
	aws cloudformation describe-stack-events --stack-name asyncdb
